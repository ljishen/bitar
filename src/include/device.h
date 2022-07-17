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

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/macros.h>
#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "config.h"
#include "memory.h"
#include "type_fwd.h"

namespace arrow {
class Buffer;
class ResizableBuffer;
}  // namespace arrow

struct rte_comp_op;

namespace magic_enum::customize {
template <typename E>
struct enum_range;
}  // namespace magic_enum::customize

namespace bitar {

namespace internal {

enum class PCIVendorId : std::uint16_t { MELLANOX = 0x15b3 };
enum class PCIDeviceId : std::uint16_t {
  /// MT42822 BlueField-2 integrated ConnectX-6 Dx network controller
  MELLANOX_CONNECTX6DXBF = 0xa2d6,
  /// MT43244 BlueField-3 integrated ConnectX-7 network controller
  MELLANOX_CONNECTX7BF = 0Xa2dc
};
enum class DeviceState {
  kUndefined = 1U << 0U,
  kConfigured = 1U << 1U,
  kStarted = 1U << 2U
};

}  // namespace internal

using PCIVendorId_MELLANOX =
    std::integral_constant<internal::PCIVendorId, internal::PCIVendorId::MELLANOX>;
using PCIDeviceId_MELLANOX_CONNECTX6DXBF =
    std::integral_constant<internal::PCIDeviceId,
                           internal::PCIDeviceId::MELLANOX_CONNECTX6DXBF>;
using PCIDeviceId_MELLANOX_CONNECTX7BF =
    std::integral_constant<internal::PCIDeviceId,
                           internal::PCIDeviceId::MELLANOX_CONNECTX7BF>;

template <typename Class,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
class CompressDevice {
 public:
  CompressDevice(const CompressDevice&) = delete;
  CompressDevice& operator=(const CompressDevice&) = delete;
  CompressDevice& operator=(CompressDevice&&) = delete;

  /// \brief Initialize this compress device with a corresponding type of configuration.
  virtual arrow::Status Initialize(std::unique_ptr<Configuration<Class>> configuration);

  /// \brief Compress a buffer with the device via the \p queue_pair_id
  /// \param[in] queue_pair_id this value must be within [0, num_qps())
  /// \param[in] decompressed_buffer the input buffer containing the data for compression
  /// \return A vector of buffers containing the compressed data
  ///
  /// Note that this function will always use the main lcore for compression, although the
  /// resources allocated for the \p queue_pair_id are used.
  arrow::Result<BufferVector> Compress(
      std::uint16_t queue_pair_id,
      const std::shared_ptr<arrow::Buffer>& decompressed_buffer);

  /// \brief Decompress buffers with the device via the \p queue_pair_id
  /// \param[in] queue_pair_id this value must be within [0, num_qps())
  /// \param[in] compressed_buffers the input buffers containing the compressed data
  /// \param[out] decompressed_buffer the buffer the decompressed data will be written to
  /// \return A status indicating whether the decompression is OK
  ///
  /// Note that this function will always use the main lcore for decompression, although
  /// the resources allocated for the \p queue_pair_id are used.
  arrow::Status Decompress(
      std::uint16_t queue_pair_id, const BufferVector& compressed_buffers,
      const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer);

  /// \brief Recycle the underlying memory resources of the buffers returned by
  /// \ref Compress "Compress()"
  /// \param[in] buffers the buffers that contain compressed data to be recycled
  /// \return the number of buffers that are recycled
  std::size_t Recycle(const BufferVector& buffers);

  /// \brief Return the lcore ID of the specified queue pair
  [[nodiscard]] auto LcoreOf(std::uint16_t queue_pair_id) const {
    return worker_lcores_.at(queue_pair_id);
  }

  /// \brief Return the device id of this device.
  [[nodiscard]] auto device_id() const noexcept { return device_id_; }

  /// \brief Return the number of queue pairs available for this device.
  [[nodiscard]] std::uint16_t num_qps() const noexcept {
    return static_cast<std::uint16_t>(worker_lcores_.size());
  }

  virtual ~CompressDevice();

 protected:
  CompressDevice(CompressDevice&&) noexcept = default;

  CompressDevice(std::uint8_t device_id, std::vector<std::uint32_t> worker_lcores);

  /// \brief Validate the configuration for this compress device.
  virtual arrow::Status ValidateConfiguration();

  /// \brief Return the generic configuration of this device.
  [[nodiscard]] const auto& configuration() const noexcept;

  virtual arrow::Status set_configuration(
      std::unique_ptr<Configuration<Class>> configuration);

  /// \brief Return the state the compress device.
  [[nodiscard]] auto state() const noexcept { return state_; }

  void set_state(internal::DeviceState state) { state_ = state; }

 private:
  /// \brief Preallocate necessary memory objects for (de)compression operations.
  arrow::Status PreAllocateMemory();

  /// \brief Perform basic checks before running (de)compress operations.
  arrow::Status EntryGuard(std::uint16_t queue_pair_id);

  /// \brief Enqueue the assembled operations to the queue pair of the device
  /// \return A status code indicating whether enqueueing is successful or not
  arrow::StatusCode EnqueueBurst(std::uint16_t queue_pair_id, rte_comp_op** enqueue_ops,
                                 std::uint16_t num_ops_assembled,
                                 QueuePairMemory<Class>* memory);

  /// \brief Dequeue operations from the device to be processed by the callback
  /// \return A status code indicating whether dequeueing is successful or not
  template <typename Callback>
  arrow::StatusCode DequeueBurst(std::uint16_t queue_pair_id, rte_comp_op** dequeue_ops,
                                 Callback&& dequeue_callback,
                                 QueuePairMemory<Class>* memory);

  /// \brief Release the memory associated with the queue pair and the compressed buffers.
  void ReleaseAll(std::uint16_t queue_pair_id, const BufferVector& buffers);

  const std::uint8_t device_id_;
  const std::vector<std::uint32_t> worker_lcores_;

  std::unique_ptr<Configuration<Class>> configuration_;
  internal::DeviceState state_{internal::DeviceState::kUndefined};

  // device_memory_ needs to be constructed before qp_memory_ and destructed after
  // qp_memory_.

  std::unique_ptr<DeviceMemory<Class>> device_memory_;
  std::vector<std::unique_ptr<QueuePairMemory<Class>>> qp_memory_;
};

class DeviceManager {
 public:
  ARROW_DISALLOW_COPY_AND_ASSIGN(DeviceManager);
  DeviceManager(DeviceManager&&) = delete;
  DeviceManager& operator=(DeviceManager&&) = delete;

  static DeviceManager* Instance();

  template <typename PCIVendorId, typename PCIDeviceId, typename Class,
            typename = internal::IsEnumConstant<internal::PCIVendorId, PCIVendorId>,
            typename = internal::IsEnumConstant<internal::PCIDeviceId, PCIDeviceId>,
            typename = internal::IsEnumConstant<internal::DriverClass, Class>>
  arrow::Result<CompressDevice<Class>*> Create(
      std::uint8_t device_id,
      std::vector<std::uint32_t> ARROW_ARG_UNUSED(worker_lcores) /*unused*/) {
    return arrow::Status::NotImplemented("Unsupported compress device ", +device_id);
  }

 private:
  DeviceManager() = default;
  ~DeviceManager() = default;
};

using MLX5CompressDevice = CompressDevice<Class_MLX5_PCI>;

template <>
arrow::Result<MLX5CompressDevice*> DeviceManager::Create<
    PCIVendorId_MELLANOX, PCIDeviceId_MELLANOX_CONNECTX6DXBF, Class_MLX5_PCI>(
    std::uint8_t device_id, std::vector<std::uint32_t> worker_lcores);

class BlueFieldCompressDevice : public MLX5CompressDevice {
  using CompressDevice::CompressDevice;

 public:
  BlueFieldCompressDevice(const BlueFieldCompressDevice&) = delete;
  BlueFieldCompressDevice& operator=(const BlueFieldCompressDevice&) = delete;
  BlueFieldCompressDevice(BlueFieldCompressDevice&&) noexcept = default;
  BlueFieldCompressDevice& operator=(BlueFieldCompressDevice&&) = delete;

  ~BlueFieldCompressDevice() override = default;

 protected:
  arrow::Status ValidateConfiguration() override;

  arrow::Status set_configuration(
      std::unique_ptr<Configuration<Class_MLX5_PCI>> configuration) override;

  friend arrow::Result<MLX5CompressDevice*> DeviceManager::Create<
      PCIVendorId_MELLANOX, PCIDeviceId_MELLANOX_CONNECTX6DXBF, Class_MLX5_PCI>(
      std::uint8_t device_id, std::vector<std::uint32_t> worker_lcores);
};

}  // namespace bitar

namespace magic_enum::customize {

// The first two structs are for changing the range of Enum values checked by
// magic_enum, requiring that (max - min + 1) must be less than UINT16_MAX.
// The third struct is to add "is_flags" to specialization enum_range.
// See https://github.com/Neargye/magic_enum/blob/master/doc/limitations.md
template <>
struct enum_range<bitar::internal::PCIVendorId> {
  static constexpr int min = 0x15b3;  // Mellanox vendor id
  static constexpr int max = 0x177d;  // Cavium vendor id
};

template <>
struct enum_range<bitar::internal::PCIDeviceId> {
  static constexpr int min = 0xa2d6;  // Mellanox BlueField-2 device id
  static constexpr int max = 0xa300;  // OCTEON TX CN83XX device id
};

template <>
struct enum_range<bitar::internal::DeviceState> {
  static constexpr bool is_flags = true;
};

}  // namespace magic_enum::customize
