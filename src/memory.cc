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

#include "memory.h"

#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/macros.h>
#include <fmt/core.h>
#include <rte_common.h>
#include <rte_comp.h>
#include <rte_compressdev.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memory.h>
#include <rte_memzone.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>  // NOLINT
#include <string>
#include <unordered_set>
#include <utility>

#include "config.h"
#include "memory_pool.h"
#include "type_fwd.h"
#include "util.h"

namespace bitar {

namespace {

constexpr auto kCacheSizeAmplifier = 2;
constexpr auto kMaxInflightMbufs = kMaxInflightOps * 16;

void ExternalBufferDummyCallback(void* ARROW_ARG_UNUSED(addr) /*unused*/,
                                 void* ARROW_ARG_UNUSED(opaque) /*unused*/) {}

arrow::Result<const rte_memzone*> AllocateOne(arrow::MemoryPool* memory_pool,
                                              std::uint16_t size) {
  auto* tracker = RtememzoneAllocatorTracker::Instance();

  std::uint8_t* addr = nullptr;
  ARROW_RETURN_NOT_OK(memory_pool->Allocate(size, &addr));
  return tracker->Of(addr);
}

__rte_always_inline void UpdateOperation(rte_comp_op* current_op, void* private_xform) {
  current_op->src.offset = 0;
  current_op->src.length = rte_pktmbuf_pkt_len(current_op->m_src);
  current_op->dst.offset = 0;
  current_op->flush_flag = RTE_COMP_FLUSH_FINAL;
  current_op->input_chksum =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<std::uintptr_t>(current_op->m_src->buf_addr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
  current_op->private_xform = private_xform;
}

}  // namespace

template <typename Class, typename Enable>
arrow::Status DeviceMemory<Class, Enable>::Preallocate() {
  auto* memory_pool = GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone);
  auto size = configuration_->compressed_seg_size();
  auto max_preallocate_memzones = configuration_->max_preallocate_memzones();

  while (memzone_pool_.size() < max_preallocate_memzones) {
    auto allocate_result = AllocateOne(memory_pool, size);
    if (allocate_result.ok()) {
      memzone_pool_.push(std::move(allocate_result).ValueUnsafe());
      continue;
    }

    auto status = allocate_result.status();
    if (!status.IsOutOfMemory()) {
      return status;
    }

    if (memzone_pool_.size() < kMinPreallocateMemzones) {
      return arrow::Status::OutOfMemory("Cannot allocate at least ",
                                        kMinPreallocateMemzones,
                                        " memzones for compress device ", +device_id_);
    }

    // It is OK to allocate memzones of less than max_preallocate_memzones
    break;
  }

  num_preallocated_memzones = static_cast<std::uint16_t>(memzone_pool_.size());
  occupied_memzones_.reserve(num_preallocated_memzones);

  RTE_LOG(INFO, USER1,
          "Successfully allocated %hu memzones of %hu bytes each (allowing to "
          "efficiently compress data of size up to %d bytes)\n",
          num_preallocated_memzones, size,
          num_preallocated_memzones * configuration_->decompressed_seg_size());

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
const rte_memzone* DeviceMemory<Class, Enable>::Take() {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (ARROW_PREDICT_TRUE(!memzone_pool_.empty())) {
    const rte_memzone* memzone = memzone_pool_.top();
    memzone_pool_.pop();
    occupied_memzones_.insert(memzone);
    return memzone;
  }

  if ((occupied_memzones_.size() - num_preallocated_memzones) %
          configuration_->burst_size() ==
      0) {
    RTE_LOG(WARNING, USER1,
            "Allocating memzones in the critical path (Performance will be impacted)\n");
  }

  auto&& allocate_result =
      AllocateOne(GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone),
                  configuration_->compressed_seg_size());
  if (ARROW_PREDICT_FALSE(!allocate_result.ok())) {
    RTE_LOG(ERR, USER1, "%s\n", allocate_result.status().ToString().c_str());
    return nullptr;
  }

  const auto* memzone = std::move(allocate_result).ValueUnsafe();
  occupied_memzones_.insert(memzone);
  return memzone;
}

template <typename Class, typename Enable>
arrow::Status DeviceMemory<Class, Enable>::Put(const std::uint8_t* addr) {
  const auto* memzone = RtememzoneAllocatorTracker::Instance()->Of(addr);
  if (memzone == nullptr) {
    return arrow::Status::Invalid(
        "Cannot find an memzone associated with virtual address ", addr);
  }

  const std::lock_guard<std::mutex> lock(mutex_);

  if (ARROW_PREDICT_FALSE(occupied_memzones_.erase(memzone) == 0)) {
    return arrow::Status::Invalid("Memzone ", memzone,
                                  " is a foreign memzone or it is not occupied");
  }

  memzone_pool_.push(memzone);
  return arrow::Status::OK();
}

template <typename Class, typename Enable>
DeviceMemory<Class, Enable>::~DeviceMemory() {
  auto* memory_pool = GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone);
  auto size = configuration_->compressed_seg_size();
  while (!memzone_pool_.empty()) {
    const auto* memzone = memzone_pool_.top();
    // NOLINTNEXTLINE
    memory_pool->Free(reinterpret_cast<std::uint8_t*>(memzone->addr), size);
    memzone_pool_.pop();
  }

  for (auto it = occupied_memzones_.begin(); it != occupied_memzones_.end();) {
    const auto* memzone = *it;
    // NOLINTNEXTLINE
    memory_pool->Free(reinterpret_cast<std::uint8_t*>(memzone->addr), size);
    it = occupied_memzones_.erase(it);
  }
}

template <typename Class, typename Enable>
DeviceMemory<Class, Enable>::DeviceMemory(
    std::uint8_t device_id, const std::unique_ptr<Configuration<Class>>& configuration)
    : device_id_{device_id}, configuration_{configuration} {}

template class DeviceMemory<Class_MLX5_PCI>;

template <typename Class, typename Enable>
arrow::Status QueuePairMemory<Class, Enable>::Preallocate() {
  auto device_socket_id = rte_compressdev_socket_id(device_id_);
  if (device_socket_id < 0) {
    return arrow::Status::Invalid("Unable to detect compress device ", +device_id_);
  }

  // A burst contains src mbufs and dst mbufs
  std::uint32_t num_mbufs_in_burst =
      configuration_->burst_size() * configuration_->max_sgl_segs() * 2U;
  std::uint32_t mbuf_pool_cache_size =
      RTE_MIN(num_mbufs_in_burst * kCacheSizeAmplifier,
              static_cast<std::uint32_t>(RTE_MEMPOOL_CACHE_MAX_SIZE));
  std::uint32_t num_inflight_mbufs =
      RTE_MIN(num_mbufs_in_burst * kMaxInflightOps, kMaxInflightMbufs);
  std::uint32_t num_mbufs_in_pool =
      num_inflight_mbufs + mbuf_pool_cache_size + num_mbufs_in_burst;

  auto mbuf_pool_name =
      fmt::format("mbuf_pool_dev_{:d}_qp_{:d}", device_id_, queue_pair_id_);
  mbuf_pool_.reset(rte_pktmbuf_pool_create(mbuf_pool_name.c_str(), num_mbufs_in_pool,
                                           mbuf_pool_cache_size, 0, 0, device_socket_id));
  if (mbuf_pool_ == nullptr) {
    return arrow::Status::Invalid("Unable to allocate mbuf pool: ", mbuf_pool_name,
                                  ". [Error ", rte_errno, ": ", rte_strerror(rte_errno),
                                  "]");
  }

  auto operation_pool_cache_size = static_cast<std::uint32_t>(RTE_MIN(
      configuration_->burst_size() * kCacheSizeAmplifier, RTE_MEMPOOL_CACHE_MAX_SIZE));
  // Round up division of `(num_mbufs_in_pool / 2) / configuration_->max_sgl_segs()`
  std::uint32_t num_ops_in_pool =
      (num_mbufs_in_pool / 2 + configuration_->max_sgl_segs() - 1) /
      configuration_->max_sgl_segs();

  auto operation_pool_name =
      fmt::format("op_pool_dev_{:d}_qp_{:d}", device_id_, queue_pair_id_);
  // For enqueue operations
  operation_pool_.reset(
      rte_comp_op_pool_create(operation_pool_name.c_str(), num_ops_in_pool,
                              operation_pool_cache_size, 0, device_socket_id));
  if (operation_pool_ == nullptr) {
    return arrow::Status::Invalid(
        "Unable to allocate operation pool: ", operation_pool_name, ". [Error ",
        rte_errno, ": ", rte_strerror(rte_errno), "]");
  }

  // For holding pointers to enqueue and dequeue operations
  operations_.reset(static_cast<rte_comp_op**>(rte_zmalloc_socket(
      nullptr,
      static_cast<std::size_t>(configuration_->burst_size() + num_ops_in_pool) *
          sizeof(void*),
      0, device_socket_id)));
  if (operations_ == nullptr) {
    return arrow::Status::Invalid(
        "Unable to allocate memory for holding pointers to compression operations");
  }

  // For holding pointers to src and dst mbufs in a single operation
  mbufs_.reset(static_cast<rte_mbuf**>(rte_zmalloc_socket(
      nullptr, num_mbufs_in_burst * sizeof(void*), 0, device_socket_id)));
  if (mbufs_ == nullptr) {
    return arrow::Status::Invalid(
        "Unable to allocate memory for holding pointers to mbufs");
  }

  // For holding the "shared" shared_info of mbufs for mapping external data
  shared_info_decompressed_mbuf_.reset(
      static_cast<rte_mbuf_ext_shared_info*>(rte_zmalloc_socket(
          nullptr, sizeof(rte_mbuf_ext_shared_info), 0, device_socket_id)));
  if (shared_info_decompressed_mbuf_ == nullptr) {
    return arrow::Status::Invalid(
        "Unable to allocate memory for the shared_info used by decompressed mbufs");
  }
  shared_info_decompressed_mbuf_->free_cb = ExternalBufferDummyCallback;
  shared_info_decompressed_mbuf_->fcb_opaque = nullptr;
  rte_mbuf_ext_refcnt_set(shared_info_decompressed_mbuf_.get(), 0);

  shared_info_compressed_mbuf_.reset(
      static_cast<rte_mbuf_ext_shared_info*>(rte_zmalloc_socket(
          nullptr, sizeof(rte_mbuf_ext_shared_info), 0, device_socket_id)));
  if (shared_info_compressed_mbuf_ == nullptr) {
    return arrow::Status::Invalid(
        "Unable to allocate memory for the shared_info used by compressed mbufs");
  }
  shared_info_compressed_mbuf_->free_cb = ExternalBufferDummyCallback;
  shared_info_compressed_mbuf_->fcb_opaque = nullptr;
  rte_mbuf_ext_refcnt_set(shared_info_compressed_mbuf_.get(), 0);

  void* private_xform = nullptr;
  auto compress_xform = configuration_->compress_xform();
  auto ret =
      rte_compressdev_private_xform_create(device_id_, &compress_xform, &private_xform);
  if (ret < 0) {
    return arrow::Status::Invalid(
        "Private xform could not be created for compression operations. [Error code: ",
        ret, "]");
  }
  private_xform_compress_.reset(private_xform);

  auto decompress_xform = configuration_->decompress_xform();
  ret =
      rte_compressdev_private_xform_create(device_id_, &decompress_xform, &private_xform);
  if (ret < 0) {
    return arrow::Status::Invalid(
        "Private xform could not be created for decompression operations. [Error code: ",
        ret, "]");
  }
  private_xform_decompress_.reset(private_xform);

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
int QueuePairMemory<Class, Enable>::AssembleFrom(
    const std::span<const std::uint8_t>& decompressed_buffer, std::uint64_t& offset) {
  auto remaining_bytes = static_cast<std::int64_t>(decompressed_buffer.size() - offset);
  auto burst_size = configuration_->burst_size();
  if (remaining_bytes <= 0 || num_ops_assembled_ >= burst_size) {
    return num_ops_assembled_;
  }

  auto next_burst_bytes_capacity = (burst_size - num_ops_assembled_) *
                                   configuration_->max_sgl_segs() *
                                   configuration_->decompressed_seg_size();
  auto next_burst_bytes = RTE_MIN(remaining_bytes, next_burst_bytes_capacity);
  auto next_burst_num_src_mbufs = static_cast<std::uint32_t>(
      (next_burst_bytes + configuration_->decompressed_seg_size() - 1) /
      configuration_->decompressed_seg_size());
  auto next_burst_num_ops = static_cast<std::uint16_t>(
      (next_burst_num_src_mbufs + configuration_->max_sgl_segs() - 1) /
      configuration_->max_sgl_segs());

  // Includes src and dst mbufs
  ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(
      BulkAllocate(next_burst_num_ops, next_burst_num_src_mbufs * 2));

  // Construct all source mbufs for the next burst operations
  std::uint32_t mbuf_id = 0;
  auto num_ops_assembled_with_mbufs = num_ops_assembled_;
  auto* current_op = operations_[num_ops_assembled_with_mbufs];
  // Save the operation early to be able to release associated resources if needed
  pending_operations_.push_back(current_op);
  while (mbuf_id < next_burst_num_src_mbufs) {
    auto buf_len = RTE_MIN(configuration_->decompressed_seg_size(),
                           decompressed_buffer.size() - offset);
    auto slice = decompressed_buffer.subspan(offset, buf_len);

    ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(AppendData(
        &current_op->m_src, mbufs_[mbuf_id],
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<std::uint8_t*>(slice.data()), rte_mem_virt2iova(slice.data()),
        static_cast<std::uint16_t>(buf_len), shared_info_decompressed_mbuf_.get()));

    ++mbuf_id;
    offset += buf_len;

    if (mbuf_id % configuration_->max_sgl_segs() == 0 &&
        mbuf_id < next_burst_num_src_mbufs) {
      current_op = operations_[++num_ops_assembled_with_mbufs];
      pending_operations_.push_back(current_op);
    }
  }

  // Construct all destination mbufs for the next burst operations
  mbuf_id = 0;
  num_ops_assembled_with_mbufs = num_ops_assembled_;
  current_op = operations_[num_ops_assembled_with_mbufs];
  while (mbuf_id < next_burst_num_src_mbufs) {
    const auto* memzone = device_memory_->Take();
    if (memzone == nullptr) {
      return -static_cast<std::underlying_type_t<arrow::StatusCode>>(
          arrow::StatusCode::IOError);
    }

    ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(
        AppendData(&current_op->m_dst, mbufs_[mbuf_id + next_burst_num_src_mbufs],
                   // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
                   memzone->addr, memzone->iova, static_cast<std::uint16_t>(memzone->len),
                   shared_info_compressed_mbuf_.get()));

    ++mbuf_id;

    if (mbuf_id % configuration_->max_sgl_segs() == 0 ||
        mbuf_id == next_burst_num_src_mbufs) {
      UpdateOperation(current_op, private_xform_compress_.get());
      current_op = operations_[++num_ops_assembled_with_mbufs];
    }
  }

  num_ops_assembled_ = num_ops_assembled_with_mbufs;

  return num_ops_assembled_;
}

template <typename Class, typename Enable>
int QueuePairMemory<Class, Enable>::AssembleFrom(
    const BufferVector& compressed_buffers, std::uint64_t& index,
    const std::span<const std::uint8_t>& decompressed_buffer, std::uint64_t& offset) {
  auto remaining_mbufs = static_cast<std::int64_t>(compressed_buffers.size() - index);
  if (remaining_mbufs <= 0 || num_ops_assembled_ >= configuration_->burst_size()) {
    return num_ops_assembled_;
  }

  auto next_burst_mbufs_capacity = (configuration_->burst_size() - num_ops_assembled_) *
                                   configuration_->max_sgl_segs();
  auto next_burst_num_src_mbufs =
      static_cast<std::uint32_t>(RTE_MIN(next_burst_mbufs_capacity, remaining_mbufs));
  auto next_burst_num_ops = static_cast<std::uint16_t>(
      (next_burst_num_src_mbufs + configuration_->max_sgl_segs() - 1) /
      configuration_->max_sgl_segs());

  // Includes src and dst mbufs
  ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(
      BulkAllocate(next_burst_num_ops, next_burst_num_src_mbufs * 2));

  auto* tracker = RtememzoneAllocatorTracker::Instance();

  // Construct all source mbufs for the next burst operations
  std::uint32_t mbuf_id = 0;
  auto num_ops_assembled_with_mbufs = num_ops_assembled_;
  auto* current_op = operations_[num_ops_assembled_with_mbufs];
  // Save the operation early to be able to release associated resources if needed
  pending_operations_.push_back(current_op);
  while (mbuf_id < next_burst_num_src_mbufs) {
    const auto& buffer = compressed_buffers[index];
    const auto* memzone = tracker->Of(buffer->data());
    if (ARROW_PREDICT_FALSE(memzone == nullptr)) {
      RTE_LOG(ERR, USER1,
              "Cannot find an memzone associated with the compressed buffer (%p)\n",
              static_cast<const void*>(buffer->data()));
      return -static_cast<std::underlying_type_t<arrow::StatusCode>>(
          arrow::StatusCode::Invalid);
    }
    ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(AppendData(
        &current_op->m_src, mbufs_[mbuf_id],
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        memzone->addr, memzone->iova, static_cast<std::uint16_t>(buffer->size()),
        shared_info_compressed_mbuf_.get()));

    ++mbuf_id;
    ++index;

    if (mbuf_id % configuration_->max_sgl_segs() == 0 &&
        mbuf_id < next_burst_num_src_mbufs) {
      current_op = operations_[++num_ops_assembled_with_mbufs];
      pending_operations_.push_back(current_op);
    }
  }

  // Construct all destination mbufs for the next burst operations
  mbuf_id = 0;
  num_ops_assembled_with_mbufs = num_ops_assembled_;
  current_op = operations_[num_ops_assembled_with_mbufs];
  auto buf_len = configuration_->decompressed_seg_size();
  while (mbuf_id < next_burst_num_src_mbufs) {
    auto slice = decompressed_buffer.subspan(offset, buf_len);

    ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(AppendData(
        &current_op->m_dst, mbufs_[mbuf_id + next_burst_num_src_mbufs],
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<std::uint8_t*>(slice.data()), rte_mem_virt2iova(slice.data()), buf_len,
        shared_info_decompressed_mbuf_.get()));

    ++mbuf_id;
    offset += buf_len;

    if (mbuf_id % configuration_->max_sgl_segs() == 0 ||
        mbuf_id == next_burst_num_src_mbufs) {
      UpdateOperation(current_op, private_xform_decompress_.get());
      current_op = operations_[++num_ops_assembled_with_mbufs];
    }
  }

  num_ops_assembled_ = num_ops_assembled_with_mbufs;

  return num_ops_assembled_;
}

template <typename Class, typename Enable>
arrow::StatusCode QueuePairMemory<Class, Enable>::AccumulateUnused(
    std::uint16_t num_unused_ops) {
  if (ARROW_PREDICT_FALSE(num_unused_ops > num_ops_assembled_)) {
    return arrow::StatusCode::Invalid;
  }

  if (num_unused_ops > 0 && num_unused_ops != num_ops_assembled_) {
    // Move the unused operations from the previous enqueue_burst call to the front to
    // maintain order
    std::memmove(operations_.get(), &operations_[num_ops_assembled_ - num_unused_ops],
                 num_unused_ops * sizeof(void*));
  }

  num_ops_assembled_ = num_unused_ops;

  return arrow::StatusCode::OK;
}

template <typename Class, typename Enable>
__rte_always_inline arrow::StatusCode QueuePairMemory<Class, Enable>::RecycleResources(
    rte_comp_op* operation) {
#ifndef NDEBUG
  if (ARROW_PREDICT_FALSE(pending_operations_.front() != operation)) {
    return arrow::StatusCode::Invalid;
  }
#endif

  pending_operations_.pop_front();

  rte_pktmbuf_free(operation->m_src);
  rte_pktmbuf_free(operation->m_dst);
  rte_comp_op_free(operation);

  return arrow::StatusCode::OK;
}

template <typename Class, typename Enable>
arrow::Status QueuePairMemory<Class, Enable>::Release() {
  while (!pending_operations_.empty()) {
    auto* current_op = pending_operations_.front();

    rte_pktmbuf_free(current_op->m_src);

    // Release the destination mbuf sequentially
    auto* dst_mbuf = current_op->m_dst;
    if (dst_mbuf != nullptr) {
      __rte_mbuf_sanity_check(dst_mbuf, 1);
    }

    while (dst_mbuf != nullptr) {
      auto* next_mbuf = dst_mbuf->next;
      // Only recycle memzones for storing compressed data
      if (dst_mbuf->shinfo == shared_info_compressed_mbuf_.get()) {
        ARROW_RETURN_NOT_OK(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            device_memory_->Put(reinterpret_cast<std::uint8_t*>(dst_mbuf->buf_addr)));
      }
      rte_pktmbuf_free_seg(dst_mbuf);
      dst_mbuf = next_mbuf;
    }

    rte_comp_op_free(current_op);

    pending_operations_.pop_front();
  }

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
rte_comp_op** QueuePairMemory<Class, Enable>::enqueue_ops() {
  if (ARROW_PREDICT_FALSE(operations_ == nullptr)) {
    return nullptr;
  }
  return operations_.get();
}

template <typename Class, typename Enable>
rte_comp_op** QueuePairMemory<Class, Enable>::dequeue_ops() {
  if (ARROW_PREDICT_FALSE(operations_ == nullptr)) {
    return nullptr;
  }
  return &operations_[configuration_->burst_size()];
}

template <typename Class, typename Enable>
QueuePairMemory<Class, Enable>::~QueuePairMemory() {
  auto status = Release();
  if (!status.ok()) {
    RTE_LOG(CRIT, USER1,
            "Failed to release memory resources for queue pair %hu of compress device "
            "%hhu. [%s]\n",
            queue_pair_id_, device_id_, status.ToString().c_str());
  }
}

template <typename Class, typename Enable>
QueuePairMemory<Class, Enable>::QueuePairMemory(
    std::uint8_t device_id, std::uint16_t queue_pair_id,
    const std::unique_ptr<Configuration<Class>>& configuration,
    const std::unique_ptr<DeviceMemory<Class>>& device_memory)
    : device_id_{device_id},
      queue_pair_id_{queue_pair_id},
      configuration_{configuration},
      device_memory_{device_memory} {}

template <typename Class, typename Enable>
arrow::StatusCode QueuePairMemory<Class, Enable>::BulkAllocate(std::uint16_t num_ops,
                                                               std::uint32_t num_mbufs) {
  if (ARROW_PREDICT_FALSE(rte_comp_op_bulk_alloc(operation_pool_.get(),
                                                 &operations_[num_ops_assembled_],
                                                 num_ops) == 0)) {
    RTE_LOG(ERR, USER1, "Not enough entries in the operation pool\n");
    return arrow::StatusCode::OutOfMemory;
  }

  if (ARROW_PREDICT_FALSE(
          rte_pktmbuf_alloc_bulk(mbuf_pool_.get(), mbufs_.get(), num_mbufs) < 0)) {
    RTE_LOG(ERR, USER1, "Not enough entries in the mbuf pool\n");
    return arrow::StatusCode::OutOfMemory;
  }

  return arrow::StatusCode::OK;
}

template <typename Class, typename Enable>
arrow::StatusCode QueuePairMemory<Class, Enable>::AppendData(
    rte_mbuf** head, rte_mbuf* tail, void* buf_addr, rte_iova_t buf_iova,
    std::uint16_t buf_len, rte_mbuf_ext_shared_info* shinfo) {
  // Chain the mbuf early to be able to release associated resources if needed
  auto* head_mbuf = *head;
  if (head_mbuf == nullptr) {
    *head = tail;
  } else {
    if (ARROW_PREDICT_FALSE(rte_pktmbuf_chain(head_mbuf, tail) != 0)) {
      RTE_LOG(ERR, USER1, "Chain segment limit exceeded\n");
      return arrow::StatusCode::CapacityError;
    }

    // Update the packet length of the head mbuf after chaining each additional mbuf
    head_mbuf->pkt_len += buf_len;
  }

  rte_pktmbuf_attach_extbuf(tail, buf_addr, buf_iova, buf_len, shinfo);
  rte_mbuf_ext_refcnt_update(shinfo, 1);

  if (ARROW_PREDICT_FALSE(rte_pktmbuf_append(tail, buf_len) == nullptr)) {
    RTE_LOG(ERR, USER1, "Not enough space in the mbuf to allocate data\n");
    return arrow::StatusCode::CapacityError;
  }

  return arrow::StatusCode::OK;
}

template class QueuePairMemory<Class_MLX5_PCI>;

}  // namespace bitar
