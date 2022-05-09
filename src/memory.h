// MIT License
//
// Copyright (c) 2022 Jianshen Liu
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stack>
#include <type_traits>
#include <unordered_set>

#include <arrow/status.h>
#include <rte_common.h>
#include <rte_compressdev.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

#include "config.h"
#include "type_fwd.h"

struct rte_comp_op;
struct rte_mbuf_ext_shared_info;
struct rte_mbuf;
struct rte_memzone;

namespace arrow_dpdk {

static inline constexpr auto kMaxInflightOps = 512;
static inline constexpr auto kMinPreallocateMemzones = 20;

template <typename, typename>
class CompressDevice;

template <typename Class,
          typename Enable = internal::IsEnumConstant<internal::DriverClass, Class>>
class DeviceMemory {
 public:
  DeviceMemory(const DeviceMemory&) = delete;
  DeviceMemory& operator=(const DeviceMemory&) = delete;
  DeviceMemory(DeviceMemory&&) noexcept = default;
  DeviceMemory& operator=(DeviceMemory&&) = delete;

  /// \brief Preallocate necessary memory resources for the device
  arrow::Status Preallocate();

  /// \brief Take a free memzone from the pool
  /// \return the pointer to the free memzone, or nullptr if not available
  const rte_memzone* Take();

  /// \brief Put the memzone associated with the virtual address \p addr back to the pool
  /// \param[in] addr the virtual address
  /// \return Status OK, or Invalid if the memzone is not found or is not from the pool
  arrow::Status Put(const std::uint8_t* addr);

  virtual ~DeviceMemory();

 private:
  DeviceMemory(std::uint8_t device_id,
               const std::shared_ptr<Configuration<Class>>& configuration);

  const std::uint8_t device_id_;
  const std::shared_ptr<Configuration<Class>>& configuration_;

  std::stack<const rte_memzone*> memzone_pool_;
  std::unordered_set<const rte_memzone*> occupied_memzones_;
  std::uint16_t num_preallocated_memzones{};

  std::mutex mutex_;

  friend class CompressDevice<Class, Enable>;
};

template <typename Class,
          typename Enable = internal::IsEnumConstant<internal::DriverClass, Class>>
class QueuePairMemory {
 public:
  QueuePairMemory(const QueuePairMemory&) = delete;
  QueuePairMemory& operator=(const QueuePairMemory&) = delete;
  QueuePairMemory(QueuePairMemory&&) noexcept = default;
  QueuePairMemory& operator=(QueuePairMemory&&) = delete;

  /// \brief Preallocate necessary memory resources for the queue pair
  arrow::Status Preallocate();

  /// \brief Assemble compression operations for the data in the \p decompressed_buffer
  /// \param[in] decompressed_buffer the input buffer containing the data for compression
  /// \param[in,out] offset the position in the buffer to start assembling from
  /// \return the number of operations assembled, or a negative arrow::StatusCode on error
  int AssembleFrom(const std::span<const std::uint8_t>& decompressed_buffer,
                   std::uint64_t& offset);

  /// \brief Assemble decompression operations
  /// \param[in] compressed_buffers the input buffers containing the compressed data
  /// \param[in,out] index the buffer at the index of the buffer vector to start from
  /// \param[out] decompressed_buffer the buffer the decompressed data will be written to
  /// \param[in,out] offset the position in the decompressed_buffer to start writing to
  /// \return the number of operations assembled, or a negative arrow::StatusCode on error
  int AssembleFrom(const BufferVector& compressed_buffers, std::uint64_t& index,
                   const std::span<const std::uint8_t>& decompressed_buffer,
                   std::uint64_t& offset);

  /// \brief Accumulate the unused operations to be used in the next enqueue request
  /// \param[in] num_unused_ops the number of unused operations
  /// \return arrow::StatusCode::OK on success, or arrow::StatusCode::Invalid if \p
  /// num_unused_ops > #num_ops_assembled_
  arrow::StatusCode AccumulateUnused(std::uint16_t num_unused_ops);

  /// \brief Recycle memory resources associated with the operation
  /// \return arrow::StatusCode::OK on success, or arrow::StatusCode::Invalid for
  /// unexpected \p operation
  arrow::StatusCode RecycleResources(rte_comp_op* operation);

  /// \brief Release memory resources associated with pending operations
  arrow::Status Release();

  /// \brief Check whether there are any pending operations that are assembled but not
  /// released
  [[nodiscard]] auto has_pending_operations() const noexcept {
    return !pending_operations_.empty();
  }

  rte_comp_op** enqueue_ops();
  rte_comp_op** dequeue_ops();

  virtual ~QueuePairMemory();

 private:
  QueuePairMemory(std::uint8_t device_id, std::uint16_t queue_pair_id,
                  const std::shared_ptr<Configuration<Class>>& configuration,
                  const std::unique_ptr<DeviceMemory<Class>>& device_memory);

  /// \return arrow::StatusCode::OK on success, or arrow::StatusCode::OutOfMemory if not
  /// enough entries in the mempool
  arrow::StatusCode BulkAllocate(std::uint16_t num_ops, std::uint32_t num_mbufs);

  arrow::StatusCode AppendData(rte_mbuf** head, rte_mbuf* tail, void* buf_addr,
                               rte_iova_t buf_iova, std::uint16_t buf_len,
                               rte_mbuf_ext_shared_info* shinfo);

  const std::uint8_t device_id_;
  const std::uint16_t queue_pair_id_;
  const std::shared_ptr<Configuration<Class>>& configuration_;
  const std::unique_ptr<DeviceMemory<Class>>& device_memory_;

  std::uint16_t num_ops_assembled_ = 0;
  // For storing enqueued but haven't been dequeued operations
  std::deque<rte_comp_op*> pending_operations_;

  struct RteMempoolDeleter {
    void operator()(rte_mempool* pool) const { rte_mempool_free(pool); }
  };

  // For allocating source and destination mbufs used by operations
  std::unique_ptr<rte_mempool, RteMempoolDeleter> mbuf_pool_;
  // For allocating operations to be enqueued to the compress device
  std::unique_ptr<rte_mempool, RteMempoolDeleter> operation_pool_;

  template <typename T>
  struct RteMemDeleter {
    void operator()(T* ptr) const { rte_free(ptr); }
  };

  // For tracking bulk allocated operations
  std::unique_ptr<rte_comp_op*[], RteMemDeleter<rte_comp_op*>> operations_;

  // For tracking bulk allocated mbufs
  std::unique_ptr<rte_mbuf*[], RteMemDeleter<rte_mbuf*>> mbufs_;

  // For mbufs to attach external buffers as data
  std::unique_ptr<rte_mbuf_ext_shared_info, RteMemDeleter<rte_mbuf_ext_shared_info>>
      shared_info_decompressed_mbuf_;
  std::unique_ptr<rte_mbuf_ext_shared_info, RteMemDeleter<rte_mbuf_ext_shared_info>>
      shared_info_compressed_mbuf_;

  struct PrivateXformDeleter {
    void operator()(void* private_xform) {
      rte_compressdev_private_xform_free(device_id, private_xform);
    }

    const std::uint8_t device_id{};
  };

  std::unique_ptr<void, PrivateXformDeleter> private_xform_compress_{
      nullptr, PrivateXformDeleter{device_id_}};
  std::unique_ptr<void, PrivateXformDeleter> private_xform_decompress_{
      nullptr, PrivateXformDeleter{device_id_}};

  friend class CompressDevice<Class, Enable>;
};

}  // namespace arrow_dpdk
