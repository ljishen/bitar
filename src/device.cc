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

#include "include/device.h"

#include <arrow/buffer.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/macros.h>
#include <fmt/format.h>
#include <rte_common.h>
#include <rte_comp.h>
#include <rte_compressdev.h>
#include <rte_config.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <utility>

#include <magic_enum.hpp>

#include "include/config.h"
#include "include/memory.h"
#include "include/util.h"

namespace bitar {

namespace {

constexpr auto kNumMaxXforms = 16;
constexpr auto kNumIgnoredMaybeNotOKBurst = 10;

arrow::Status ValidateWindowSize(std::uint8_t window_size,
                                 const rte_param_log2_range& range) {
  // Check lower/upper bounds
  if (window_size < range.min || window_size > range.max) {
    return arrow::Status::Invalid("window_size is not in the range of [", range.min, ", ",
                                  range.max, "]");
  }

  // If range is actually only one value, window_size is correct
  if (range.increment == 0) {
    return arrow::Status::OK();
  }

  // Check if value is one of the supported sizes
  for (std::uint8_t next_size = range.min; next_size <= range.max;
       next_size += range.increment) {
    if (window_size == next_size) {
      return arrow::Status::OK();
    }
  }

  return arrow::Status::Invalid("The value of window_size is invalid");
}

__rte_always_inline std::uint64_t GetErrorCount(std::uint16_t num_bursted_ops,
                                                std::uint8_t device_id) {
  static std::uint32_t num_maybe_not_ok = 0;

  if (num_bursted_ops > 0) {
    num_maybe_not_ok = 0;
    return 0;
  }

  if (++num_maybe_not_ok % kNumIgnoredMaybeNotOKBurst != 0) {
    return 0;
  }

  static rte_compressdev_stats stats;
  rte_compressdev_stats_get(device_id, &stats);

  if (ARROW_PREDICT_FALSE(stats.enqueue_err_count > 0)) {
    return stats.enqueue_err_count;
  }

  if (ARROW_PREDICT_FALSE(stats.dequeue_err_count > 0)) {
    return stats.dequeue_err_count;
  }

  num_maybe_not_ok = 0;
  return 0;
}

}  // namespace

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::Initialize(
    std::unique_ptr<Configuration<Class>> configuration) {
  ARROW_RETURN_NOT_OK(set_configuration(std::move(configuration)));
  ARROW_RETURN_NOT_OK(ValidateConfiguration());
  ARROW_RETURN_NOT_OK(PreAllocateMemory());

  auto socket_id = rte_compressdev_socket_id(device_id_);

  rte_compressdev_config compressdev_config{.socket_id = socket_id,
                                            .nb_queue_pairs = num_qps(),
                                            .max_nb_priv_xforms = kNumMaxXforms,
                                            .max_nb_streams = 0};

  if (rte_compressdev_configure(device_id_, &compressdev_config) < 0) {
    return arrow::Status::Invalid("Device configuration failed");
  }
  set_state(internal::DeviceState::kConfigured);

  int ret = 0;
  std::vector<std::string> qp_to_lcore;
  for (std::uint16_t qp_id = 0; qp_id < num_qps(); ++qp_id) {
    ret = rte_compressdev_queue_pair_setup(device_id_, qp_id, kMaxInflightOps, socket_id);
    if (ret < 0) {
      return arrow::Status::Invalid("Failed to setup queue pair ", qp_id, " for device ",
                                    device_id_, ". [Error code: ", ret, "]");
    }
    qp_to_lcore.emplace_back(fmt::format("[{:d}->{:d}]", qp_id, LcoreOf(qp_id)));
  }
  RTE_LOG(
      INFO, USER1, "%s\n",
      fmt::format(
          "Compress device {:d} has set up the following [queue_pair_id -> lcore_id]: {}",
          device_id_, fmt::join(qp_to_lcore, ", "))
          .c_str());

  ret = rte_compressdev_start(device_id_);
  if (ret < 0) {
    return arrow::Status::Invalid("Failed to start device ", +device_id_,
                                  ". [Error code: ", ret, "]");
  }
  set_state(internal::DeviceState::kStarted);

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
arrow::Result<BufferVector> CompressDevice<Class, Enable>::Compress(
    std::uint16_t queue_pair_id,
    const std::unique_ptr<arrow::Buffer>& decompressed_buffer) {
  BufferVector compressed_buffers;
  if (ARROW_PREDICT_FALSE(decompressed_buffer == nullptr ||
                          decompressed_buffer->size() == 0)) {
    return compressed_buffers;
  }

  ARROW_RETURN_NOT_OK(EntryGuard(queue_pair_id));

  auto num_compressed_buffers = static_cast<std::size_t>(
      (decompressed_buffer->size() + configuration_->decompressed_seg_size() - 1) /
      configuration_->decompressed_seg_size());
  compressed_buffers.reserve(num_compressed_buffers);

  std::uint64_t offset = 0;
  int num_ops_assembled = 0;

  auto* memory = qp_memory_[queue_pair_id].get();
  auto** enqueue_ops = memory->enqueue_ops();
  auto** dequeue_ops = memory->dequeue_ops();

  std::span<const std::uint8_t> decompressed_buffer_span{
      decompressed_buffer->data(),
      static_cast<std::uint64_t>(decompressed_buffer->size())};

  auto dequeue_callback = [&compressed_buffers](rte_comp_op* current_op) {
    auto* dst_mbuf = current_op->m_dst;
    std::uint32_t remaining_length = current_op->produced;
    while (remaining_length > 0) {
      const auto* buf_addr = rte_pktmbuf_mtod(dst_mbuf, std::uint8_t*);
      const auto data_len = RTE_MIN(rte_pktmbuf_data_len(dst_mbuf), remaining_length);
      compressed_buffers.emplace_back(
          std::make_unique<arrow::Buffer>(buf_addr, data_len));

      remaining_length -= data_len;
      dst_mbuf = dst_mbuf->next;
    }
  };

  ARROW_ASSIGN_OR_RAISE_NEGATIVE_ELSE(
      num_ops_assembled, memory->AssembleFrom(decompressed_buffer_span, offset),
      fmt::format("Failed to assemble compression operations for queue pair {:d} of "
                  "compress device {:d}",
                  queue_pair_id, device_id_),
      ReleaseAll(queue_pair_id, compressed_buffers));

  while (num_ops_assembled > 0) {
    ARROW_RETURN_NOT_STATUS_OK_ELSE(
        EnqueueBurst(queue_pair_id, enqueue_ops,
                     static_cast<std::uint16_t>(num_ops_assembled), memory),
        fmt::format("Failed to enqueue compression operations to compress device {:d} "
                    "via queue pair {:d}",
                    device_id_, queue_pair_id),
        ReleaseAll(queue_pair_id, compressed_buffers));

    ARROW_RETURN_NOT_STATUS_OK_ELSE(
        DequeueBurst(queue_pair_id, dequeue_ops, dequeue_callback, memory),
        fmt::format("Failed to dequeue compression operations from compress device {:d} "
                    "via queue pair {:d}",
                    device_id_, queue_pair_id),
        ReleaseAll(queue_pair_id, compressed_buffers));

    ARROW_ASSIGN_OR_RAISE_NEGATIVE_ELSE(
        num_ops_assembled, memory->AssembleFrom(decompressed_buffer_span, offset),
        fmt::format("Failed to assemble compression operations for queue pair {:d} of "
                    "compress device {:d}",
                    queue_pair_id, device_id_),
        ReleaseAll(queue_pair_id, compressed_buffers));
  }

  while (memory->has_pending_operations()) {
    ARROW_RETURN_NOT_STATUS_OK_ELSE(
        DequeueBurst(queue_pair_id, dequeue_ops, dequeue_callback, memory),
        fmt::format(
            "Failed to dequeue pending compression operations from compress device {:d} "
            "via queue pair {:d}",
            device_id_, queue_pair_id),
        ReleaseAll(queue_pair_id, compressed_buffers));
  }

  return compressed_buffers;
}

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::Decompress(
    std::uint16_t queue_pair_id, const BufferVector& compressed_buffers,
    const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer) {
  if (ARROW_PREDICT_FALSE(compressed_buffers.empty())) {
    return arrow::Status::OK();
  }

  auto min_decompressed_buffer_capacity = static_cast<std::int64_t>(
      compressed_buffers.size() * configuration_->decompressed_seg_size());
  if (decompressed_buffer == nullptr ||
      decompressed_buffer->capacity() < min_decompressed_buffer_capacity) {
    return arrow::Status::CapacityError("The decompressed_buffer is required to be >= ",
                                        min_decompressed_buffer_capacity, " bytes");
  }

  ARROW_RETURN_NOT_OK(EntryGuard(queue_pair_id));

  std::uint64_t index = 0;
  std::uint64_t offset = 0;
  int num_ops_assembled = 0;
  std::int64_t decompressed_buffer_size = 0;

  auto* memory = qp_memory_[queue_pair_id].get();
  auto** enqueue_ops = memory->enqueue_ops();
  auto** dequeue_ops = memory->dequeue_ops();

  std::span<const std::uint8_t> decompressed_buffer_span{
      decompressed_buffer->data(),
      static_cast<std::uint64_t>(decompressed_buffer->capacity())};

  auto dequeue_callback = [&decompressed_buffer_size](rte_comp_op* current_op) {
    decompressed_buffer_size += current_op->produced;
  };

  ARROW_ASSIGN_OR_RAISE_NEGATIVE(
      num_ops_assembled,
      memory->AssembleFrom(compressed_buffers, index, decompressed_buffer_span, offset),
      fmt::format("Failed to assemble decompression operations for queue pair {:d} of "
                  "compress device {:d}",
                  queue_pair_id, device_id_));

  while (num_ops_assembled > 0) {
    ARROW_RETURN_NOT_STATUS_OK(
        EnqueueBurst(queue_pair_id, enqueue_ops,
                     static_cast<std::uint16_t>(num_ops_assembled), memory),
        fmt::format("Failed to enqueue decompression operations to compress device {:d} "
                    "via queue pair {:d}",
                    device_id_, queue_pair_id));

    ARROW_RETURN_NOT_STATUS_OK(
        DequeueBurst(queue_pair_id, dequeue_ops, dequeue_callback, memory),
        fmt::format(
            "Failed to dequeue decompression operations from compress device {:d} "
            "via queue pair {:d}",
            device_id_, queue_pair_id));

    ARROW_ASSIGN_OR_RAISE_NEGATIVE(
        num_ops_assembled,
        memory->AssembleFrom(compressed_buffers, index, decompressed_buffer_span, offset),
        fmt::format("Failed to assemble decompression operations for queue pair {:d} of "
                    "compress device {:d}",
                    queue_pair_id, device_id_));
  }

  while (memory->has_pending_operations()) {
    ARROW_RETURN_NOT_STATUS_OK(
        DequeueBurst(queue_pair_id, dequeue_ops, dequeue_callback, memory),
        fmt::format("Failed to dequeue pending decompression operations from compress "
                    "device {:d} via queue pair {:d}",
                    device_id_, queue_pair_id));
  }

  // Don't shrink the buffer for performance purposes. The result is ignored because
  // calling this function will be successful with an nonnegative value as the argument of
  // new_size
  ARROW_UNUSED(decompressed_buffer->Resize(decompressed_buffer_size, false));

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
std::size_t CompressDevice<Class, Enable>::Recycle(const BufferVector& buffers) {
  std::size_t count = 0;
  for (auto it = std::crbegin(buffers); it != std::crend(buffers); ++it) {
    count += device_memory_->Put((*it)->data());
  }
  return count;
}

template <typename Class, typename Enable>
CompressDevice<Class, Enable>::~CompressDevice() {
  if (magic_enum::enum_integer(state()) >=
      magic_enum::enum_integer(internal::DeviceState::kStarted)) {
    // this function needs to be called before rte_compressdev_close()
    rte_compressdev_stop(device_id_);
    set_state(internal::DeviceState::kConfigured);
  }

  if (magic_enum::enum_integer(state()) >=
      magic_enum::enum_integer(internal::DeviceState::kConfigured)) {
    rte_compressdev_close(device_id_);
    set_state(internal::DeviceState::kUndefined);
  }
}

template <typename Class, typename Enable>
CompressDevice<Class, Enable>::CompressDevice(std::uint8_t device_id,
                                              std::vector<std::uint32_t> worker_lcores)
    : device_id_(device_id), worker_lcores_(std::move(worker_lcores)) {
  qp_memory_.reserve(num_qps());
}

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::ValidateConfiguration() {
  rte_compressdev_info device_info{};
  rte_compressdev_info_get(device_id_, &device_info);
  if (device_info.max_nb_queue_pairs > 0 && num_qps() > device_info.max_nb_queue_pairs) {
    return arrow::Status::Invalid(
        "The requested number of queue pairs (", num_qps(), ") exceeds the maximum (",
        device_info.max_nb_queue_pairs, ") allowed for device ", +device_id_);
  }

  if (configuration_->burst_size() == 0) {
    return arrow::Status::Invalid("Burst size must be greater than 0");
  }

  const auto* capability =
      rte_compressdev_capability_get(device_id_, RTE_COMP_ALGO_DEFLATE);
  if (capability == nullptr) {
    return arrow::Status::NotImplemented("Compress device ", +device_id_,
                                         " does not support DEFLATE");
  }

  if (configuration_->max_sgl_segs() < 1) {
    configuration_->set_max_sgl_segs(1);
  }

  if (configuration_->max_sgl_segs() > 1 &&
      (capability->comp_feature_flags & RTE_COMP_FF_OOP_SGL_IN_SGL_OUT) == 0) {
    return arrow::Status::Invalid("Compress device does not support chained mbufs.");
  }

  if (configuration_->decompressed_seg_size() < internal::kMinSegSize ||
      configuration_->decompressed_seg_size() > internal::kMaxSegSize) {
    return arrow::Status::Invalid("decompressed_seg_size is not in the range of [",
                                  internal::kMinSegSize, ", ", internal::kMaxSegSize,
                                  "]");
  }

  if (configuration_->window_size() == 0) {
    configuration_->set_window_size(capability->window_size.max);
  } else {
    ARROW_RETURN_NOT_OK(
        ValidateWindowSize(configuration_->window_size(), capability->window_size));
  }

  if (configuration_->huffman_enc() == RTE_COMP_HUFFMAN_FIXED &&
      (capability->comp_feature_flags & RTE_COMP_FF_HUFFMAN_FIXED) == 0) {
    return arrow::Status::Invalid("Compress device does not supported fixed Huffman");
  }

  if (configuration_->huffman_enc() == RTE_COMP_HUFFMAN_DYNAMIC &&
      (capability->comp_feature_flags & RTE_COMP_FF_HUFFMAN_DYNAMIC) == 0) {
    return arrow::Status::Invalid("Compress device does not supported dynamic Huffman");
  }

  if (configuration_->max_preallocate_memzones() < kMinPreallocateMemzones ||
      configuration_->max_preallocate_memzones() > RTE_MAX_MEMZONE) {
    return arrow::Status::Invalid("max_preallocate_memzones is not in the range of [",
                                  kMinPreallocateMemzones, ", ", RTE_MAX_MEMZONE, "]");
  }

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
const auto& CompressDevice<Class, Enable>::configuration() const noexcept {
  return configuration_;
}

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::set_configuration(
    std::unique_ptr<Configuration<Class>> configuration) {
  configuration_ = std::move(configuration);
  return arrow::Status::OK();
}

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::PreAllocateMemory() {
  device_memory_ = std::unique_ptr<DeviceMemory<Class>>(
      new DeviceMemory<Class>(device_id_, configuration_));
  ARROW_RETURN_NOT_OK(device_memory_->Preallocate());

  for (std::uint16_t qp_id = 0; qp_id < num_qps(); ++qp_id) {
    qp_memory_.emplace_back(std::unique_ptr<QueuePairMemory<Class>>(
        new QueuePairMemory<Class>(device_id_, qp_id, configuration_, device_memory_)));
    ARROW_RETURN_NOT_OK(qp_memory_.back()->Preallocate());
  }
  return arrow::Status::OK();
}

template <typename Class, typename Enable>
arrow::Status CompressDevice<Class, Enable>::EntryGuard(std::uint16_t queue_pair_id) {
  if (ARROW_PREDICT_FALSE(state_ < internal::DeviceState::kStarted)) {
    return arrow::Status::Invalid(
        "Compress device ", +device_id_,
        " has not started. [Current state: ", magic_enum::enum_name(state_), "]");
  }

  if (ARROW_PREDICT_FALSE(queue_pair_id >= num_qps())) {
    return arrow::Status::Invalid("queue_pair_id must be in the range of [0, ", num_qps(),
                                  ")");
  }

  if (qp_memory_[queue_pair_id]->has_pending_operations()) {
    return arrow::Status::Cancelled("Queue pair ", queue_pair_id, " of compress device ",
                                    device_id_, " is busy");
  }

  return arrow::Status::OK();
}

template <typename Class, typename Enable>
arrow::StatusCode CompressDevice<Class, Enable>::EnqueueBurst(
    std::uint16_t queue_pair_id, rte_comp_op** enqueue_ops,
    std::uint16_t num_ops_assembled, QueuePairMemory<Class>* memory) {
  auto num_enqueued = rte_compressdev_enqueue_burst(device_id_, queue_pair_id,
                                                    enqueue_ops, num_ops_assembled);

  auto errors = GetErrorCount(num_enqueued, device_id_);
  if (ARROW_PREDICT_FALSE(errors > 0)) {
    RTE_LOG(ERR, USER1,
            "Unable to enqueue operations to compress device %hhu via queue pair %hu. "
            "[Error count: %lu]\n",
            device_id_, queue_pair_id, errors);
    return arrow::StatusCode::IOError;
  }

  std::uint16_t num_unused_ops = num_ops_assembled - num_enqueued;
  auto status_code = memory->AccumulateUnused(num_unused_ops);
  if (ARROW_PREDICT_FALSE(status_code != arrow::StatusCode::OK)) {
    RTE_LOG(ERR, USER1, "Less than %hu operations was assembled\n", num_unused_ops);
    return status_code;
  }

  return arrow::StatusCode::OK;
}

template <typename Class, typename Enable>
template <typename Callback>
arrow::StatusCode CompressDevice<Class, Enable>::DequeueBurst(
    std::uint16_t queue_pair_id, rte_comp_op** dequeue_ops, Callback&& dequeue_callback,
    QueuePairMemory<Class>* memory) {
  auto num_dequeued = rte_compressdev_dequeue_burst(
      device_id_, queue_pair_id, dequeue_ops, configuration_->burst_size());

  auto errors = GetErrorCount(num_dequeued, device_id_);
  if (ARROW_PREDICT_FALSE(errors > 0)) {
    RTE_LOG(ERR, USER1,
            "Unable to dequeue operations from compress device %hhu via queue pair %hu. "
            "[Error count: %lu]\n",
            device_id_, queue_pair_id, errors);
    return arrow::StatusCode::IOError;
  }

  std::uint16_t idx = 0;
  while (idx < num_dequeued) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto* current_op = dequeue_ops[idx];

    if (ARROW_PREDICT_FALSE(current_op->status != RTE_COMP_OP_STATUS_SUCCESS)) {
      if (current_op->status == RTE_COMP_OP_STATUS_OUT_OF_SPACE_TERMINATED ||
          current_op->status == RTE_COMP_OP_STATUS_OUT_OF_SPACE_RECOVERABLE) {
        RTE_LOG(ERR, USER1, "Compress data output is larger than allocated buffer\n");
      } else {
        RTE_LOG(ERR, USER1, "Some operations have failed\n");
      }
      return arrow::StatusCode::IOError;
    }

    dequeue_callback(current_op);

    auto status_code = memory->RecycleResources(current_op);
    if (ARROW_PREDICT_FALSE(status_code != arrow::StatusCode::OK)) {
      RTE_LOG(ERR, USER1, "Unable to recycle unexpected operation (%p)\n",
              static_cast<void*>(current_op));
      return status_code;
    }

    ++idx;
  }

  return arrow::StatusCode::OK;
}

template <typename Class, typename Enable>
void CompressDevice<Class, Enable>::ReleaseAll(std::uint16_t queue_pair_id,
                                               const BufferVector& buffers) {
  ARROW_UNUSED(Recycle(buffers));
  qp_memory_[queue_pair_id]->Release();
}

template class CompressDevice<Class_MLX5_PCI>;

DeviceManager* DeviceManager::Instance() {
  static DeviceManager instance;
  return &instance;
}

template <>
arrow::Result<MLX5CompressDevice*> DeviceManager::Create<
    PCIVendorId_MELLANOX, PCIDeviceId_MELLANOX_CONNECTX6DXBF, Class_MLX5_PCI>(
    std::uint8_t device_id, std::vector<std::uint32_t> worker_lcores) {
  return new BlueFieldCompressDevice(device_id, std::move(worker_lcores));
}

arrow::Status BlueFieldCompressDevice::ValidateConfiguration() {
  const auto* bluefield_configuration =
      dynamic_cast<BlueFieldConfiguration*>(configuration().get());

  /// BlueField device only supports RTE_COMP_HUFFMAN_FIXED and
  /// RTE_COMP_HUFFMAN_DYNAMIC
  if (bluefield_configuration->huffman_enc() != RTE_COMP_HUFFMAN_FIXED &&
      bluefield_configuration->huffman_enc() != RTE_COMP_HUFFMAN_DYNAMIC) {
    return arrow::Status::Invalid("Compress device of type ",
                                  bluefield_configuration->type_name(),
                                  " only supports huffman encoding of either "
                                  "RTE_COMP_HUFFMAN_FIXED or RTE_COMP_HUFFMAN_DYNAMIC");
  }

  return MLX5CompressDevice::ValidateConfiguration();
}

arrow::Status BlueFieldCompressDevice::set_configuration(
    std::unique_ptr<Configuration<Class_MLX5_PCI>> configuration) {
  if (configuration->type_name() != kBlueFieldConfigurationTypeName) {
    return arrow::Status::Invalid("Configuration of type ", configuration->type_name(),
                                  " cannot be applied to compress device of type ",
                                  kBlueFieldConfigurationTypeName);
  }

  return MLX5CompressDevice::set_configuration(std::move(configuration));
}

}  // namespace bitar
