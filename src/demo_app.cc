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

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/type_fwd.h>
#include <arrow/ipc/options.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>
#include <arrow/util/macros.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <cxxopts.hpp>
#include <ext/alloc_traits.h>

#include "common.h"
#include "config.h"
#include "device.h"
#include "driver.h"
#include "memory_pool.h"
#include "type_fwd.h"
#include "util.h"

#ifdef __aarch64__
#include <rte_memcpy_64.h>
#else
#include <rte_memcpy.h>
#endif

constexpr auto kBitsPerByte = 8;
constexpr auto kGigabit = 1e9;
constexpr auto kMicroseconds = 1e6;

constexpr auto kDecompressedSegSize = 59460;
constexpr auto kBurstSize = 32 * 1;
constexpr auto kNumTests = 3;

std::uint32_t num_parallel_tests();

arrow::Result<std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>>
GetBlueFieldCompressDevices(std::uint64_t max_buffer_size);

arrow::Result<arrow::BufferVector> ReadBuffers(const char* ipc_file_path);

arrow::Result<std::unique_ptr<arrow::Buffer>> ReadFileBuffer(
    const std::string& ipc_file_path, std::int64_t num_bytes);

arrow::Status Release(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector);

void Advance(std::uint8_t& device_id, std::uint16_t& queue_pair_id,
             std::uint16_t num_qps);

arrow::Result<arrow_dpdk::BufferVector> BenchmarkCompressSync(
    const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
    std::uint16_t queue_pair_id,
    const std::unique_ptr<arrow::Buffer>& decompressed_buffer);

arrow::Status BenchmarkDecompressSync(
    const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
    std::uint16_t queue_pair_id, const arrow_dpdk::BufferVector& compressed_buffers,
    const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer);

arrow::Status BenchmarkCompressAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const arrow_dpdk::BufferVector& input_buffer_vector,
    std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector);

arrow::Status BenchmarkDecompressAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector,
    const std::vector<std::unique_ptr<arrow::ResizableBuffer>>&
        decompressed_buffer_vector);

arrow::Status EvaluateSync(const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
                           const std::unique_ptr<arrow::Buffer>& input_buffer);

arrow::Status EvaluateAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unique_ptr<arrow::Buffer>& input_buffer);

arrow::Status Evaluate(const std::unique_ptr<arrow::Buffer>& input_buffer);

void SignalHandler(int signal);

std::uint32_t num_parallel_tests() {
  static std::uint32_t kNumParallelTests = rte_lcore_count() - 1;
  return kNumParallelTests;
}

arrow::Result<std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>>
GetBlueFieldCompressDevices(std::uint64_t max_buffer_size) {
  auto* driver = arrow_dpdk::CompressDriver<arrow_dpdk::Class_MLX5_PCI>::Instance();
  ARROW_ASSIGN_OR_RAISE(auto device_ids, driver->ListAvailableDeviceIds());

  RTE_LOG(INFO, USER1, "Found devices with MLX5 driver: [%s]\n",
          fmt::format("{}", fmt::join(device_ids, ", ")).c_str());

  ARROW_ASSIGN_OR_RAISE(auto devices, driver->GetDevices(device_ids));
  auto bluefield_config = std::make_shared<arrow_dpdk::BlueFieldConfiguration>(
      arrow_dpdk::BlueFieldConfiguration::Defaults());
  bluefield_config->set_decompressed_seg_size(kDecompressedSegSize);
  bluefield_config->set_burst_size(kBurstSize);

  auto max_preallocate_memzones_total =
      (max_buffer_size + bluefield_config->decompressed_seg_size() - 1) /
      bluefield_config->decompressed_seg_size() * num_parallel_tests();
  auto max_preallocate_memzones = static_cast<std::uint16_t>(
      (max_preallocate_memzones_total + devices.size() - 1) / devices.size());
  bluefield_config->set_max_preallocate_memzones(max_preallocate_memzones);

  for (auto& device : devices) {
    if (dynamic_cast<arrow_dpdk::BlueFieldCompressDevice*>(device.get()) == nullptr) {
      return arrow::Status::Invalid("Compress device ", device->device_id(),
                                    " is not a BlueField device");
    }
    ARROW_RETURN_NOT_OK(device->Initialize(bluefield_config));
  }

  return devices;
}

arrow::Result<arrow::BufferVector> ReadBuffers(const char* ipc_file_path) {
  ARROW_ASSIGN_OR_RAISE(auto file, arrow::io::MemoryMappedFile::Open(
                                       ipc_file_path, arrow::io::FileMode::READ));
  ARROW_ASSIGN_OR_RAISE(auto reader,
                        arrow::ipc::RecordBatchFileReader::Open(
                            std::move(file), arrow::ipc::IpcReadOptions::Defaults()));

  auto num_batches = reader->num_record_batches();
  arrow::BufferVector buffers;
  buffers.reserve(static_cast<std::size_t>(num_batches));

  auto write_options = arrow::ipc::IpcWriteOptions::Defaults();
  write_options.memory_pool =
      arrow_dpdk::GetMemoryPool(arrow_dpdk::MemoryPoolBackend::Rtememzone);

  for (int i = 0; i < num_batches; ++i) {
    ARROW_ASSIGN_OR_RAISE(auto batch, reader->ReadRecordBatch(i));
    ARROW_ASSIGN_OR_RAISE(auto buffer,
                          arrow::ipc::SerializeRecordBatch(*batch, write_options));
    buffers.emplace_back(std::move(buffer));
  }

  return buffers;
}

arrow::Result<std::unique_ptr<arrow::Buffer>> ReadFileBuffer(
    const std::string& ipc_file_path, std::int64_t num_bytes) {
  ARROW_ASSIGN_OR_RAISE(auto file, arrow::io::MemoryMappedFile::Open(
                                       ipc_file_path, arrow::io::FileMode::READ));

  ARROW_ASSIGN_OR_RAISE(
      auto buffer,
      arrow::AllocateBuffer(num_bytes, arrow_dpdk::GetMemoryPool(
                                           arrow_dpdk::MemoryPoolBackend::Rtememzone)));

  ARROW_ASSIGN_OR_RAISE(auto num_bytes_read,
                        file->Read(num_bytes, buffer->mutable_data()));
  if (num_bytes_read != num_bytes) {
    return arrow::Status::IOError("Unable to read ", num_bytes, " bytes from file");
  }

  return buffer;
}

arrow::Status Release(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector) {
  for (const auto& [device_id, compressed_buffers_vector] :
       device_to_compressed_buffers_vector) {
    for (const auto& compressed_buffers : compressed_buffers_vector) {
      ARROW_RETURN_NOT_OK(devices[device_id]->Release(compressed_buffers));
    }
  }

  return arrow::Status::OK();
}

inline void Advance(std::uint8_t& device_id, std::uint16_t& queue_pair_id,
                    std::uint16_t num_qps) {
  ++queue_pair_id;
  if (queue_pair_id == num_qps) {
    queue_pair_id = 0;
    ++device_id;
  }
}

arrow::Result<arrow_dpdk::BufferVector> BenchmarkCompressSync(
    const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
    std::uint16_t queue_pair_id,
    const std::unique_ptr<arrow::Buffer>& decompressed_buffer) {
  auto compression_start = std::chrono::high_resolution_clock::now();

  ARROW_ASSIGN_OR_RAISE(auto compressed_buffers,
                        device->Compress(queue_pair_id, decompressed_buffer));

  auto compression_duration =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::high_resolution_clock::now() - compression_start)
          .count();
  fmt::print("-> Duration: {:d} microseconds\t\tThroughput: {:.2f} Gbps\n",
             compression_duration,
             static_cast<double>(decompressed_buffer->size()) * kBitsPerByte / kGigabit /
                 static_cast<double>(compression_duration) * kMicroseconds);

  return compressed_buffers;
}

arrow::Status BenchmarkDecompressSync(
    const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
    std::uint16_t queue_pair_id, const arrow_dpdk::BufferVector& compressed_buffers,
    const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer) {
  auto decompression_start = std::chrono::high_resolution_clock::now();

  ARROW_RETURN_NOT_OK(
      device->Decompress(queue_pair_id, compressed_buffers, decompressed_buffer));

  auto decompression_duration =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::high_resolution_clock::now() - decompression_start)
          .count();
  fmt::print("-> Duration: {:d} microseconds\t\tThroughput: {:.2f} Gbps\n",
             decompression_duration,
             static_cast<double>(decompressed_buffer->size()) * kBitsPerByte / kGigabit /
                 static_cast<double>(decompression_duration) * kMicroseconds);

  return arrow::Status::OK();
}

arrow::Status BenchmarkCompressAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const arrow_dpdk::BufferVector& input_buffer_vector,
    std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector) {
  std::chrono::time_point<std::chrono::high_resolution_clock> compression_end;

  auto compress_result_callback =
      [&](std::uint8_t device_id, std::uint16_t queue_pair_id,
          arrow::Result<arrow_dpdk::BufferVector>&& result) -> int {
    if (!result.ok()) {
      RTE_LOG(ERR, USER1,
              "Failed to complete async compression via queue pair %hu of compress "
              "device %hhu. [%s]\n",
              queue_pair_id, device_id, result.status().ToString().c_str());
      return EXIT_FAILURE;
    }
    compression_end = std::chrono::high_resolution_clock::now();

    device_to_compressed_buffers_vector.at(device_id)[queue_pair_id] =
        std::move(result).ValueUnsafe();
    return arrow_dpdk::kAsyncReturnOK;
  };

  using CompressParamType = arrow_dpdk::CompressParam<arrow_dpdk::Class_MLX5_PCI,
                                                      decltype(compress_result_callback)>;

  std::vector<std::unique_ptr<CompressParamType>> compress_param_vector;
  compress_param_vector.reserve(num_parallel_tests());
  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;

  auto compression_start = std::chrono::high_resolution_clock::now();

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    compress_param_vector.emplace_back(std::make_unique<CompressParamType>(
        device, queue_pair_id, input_buffer_vector[idx], compress_result_callback));
    if (arrow_dpdk::CompressAsync(compress_param_vector[idx]) != 0) {
      break;
    }

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  device_id = 0;
  queue_pair_id = 0;
  bool async_success = true;

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    int ret = rte_eal_wait_lcore(device->LcoreOf(queue_pair_id));
    if (ret == 0) {
      RTE_LOG(ERR, USER1,
              "Unable to start async compression for queue pair %hu of compress device "
              "%hhu.\n",
              queue_pair_id, device_id);
    }
    async_success &= ret == arrow_dpdk::kAsyncReturnOK;

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  // Avoid missing the opportunity to release by waiting till results from all worker
  // lcores are known
  if (!async_success) {
    ARROW_UNUSED(Release(devices, device_to_compressed_buffers_vector));
    return arrow::Status::IOError("Failed to complete async compression");
  }

  auto compression_duration = std::chrono::duration_cast<std::chrono::microseconds>(
                                  compression_end - compression_start)
                                  .count();
  fmt::print("-> Duration: {:d} microseconds\t\tThroughput: {:.2f} Gbps\n",
             compression_duration,
             num_parallel_tests() *
                 static_cast<double>(input_buffer_vector.front()->size()) * kBitsPerByte /
                 kGigabit / static_cast<double>(compression_duration) * kMicroseconds);

  return arrow::Status::OK();
}

arrow::Status BenchmarkDecompressAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>&
        device_to_compressed_buffers_vector,
    const std::vector<std::unique_ptr<arrow::ResizableBuffer>>&
        decompressed_buffer_vector) {
  std::chrono::time_point<std::chrono::high_resolution_clock> decompression_end;

  auto decompress_result_callback = [&](std::uint8_t device_id,
                                        std::uint16_t queue_pair_id,
                                        const arrow::Status& status) -> int {
    if (!status.ok()) {
      RTE_LOG(ERR, USER1,
              "Failed to complete async decompression via queue pair %hu of compress "
              "device %hhu. [%s]\n",
              queue_pair_id, device_id, status.ToString().c_str());
      return EXIT_FAILURE;
    }
    decompression_end = std::chrono::high_resolution_clock::now();
    return arrow_dpdk::kAsyncReturnOK;
  };

  using DecompressParamType =
      arrow_dpdk::DecompressParam<arrow_dpdk::Class_MLX5_PCI,
                                  decltype(decompress_result_callback)>;

  std::vector<std::unique_ptr<DecompressParamType>> decompress_param_vector;
  decompress_param_vector.reserve(num_parallel_tests());
  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;

  auto decompression_start = std::chrono::high_resolution_clock::now();

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    decompress_param_vector.emplace_back(std::make_unique<DecompressParamType>(
        device, queue_pair_id,
        device_to_compressed_buffers_vector.at(device_id)[queue_pair_id],
        decompressed_buffer_vector[idx], decompress_result_callback));
    if (arrow_dpdk::DecompressAsync(decompress_param_vector[idx]) != 0) {
      break;
    }

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  device_id = 0;
  queue_pair_id = 0;
  bool async_success = true;

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    int ret = rte_eal_wait_lcore(device->LcoreOf(queue_pair_id));
    if (ret == 0) {
      RTE_LOG(ERR, USER1,
              "Unable to start async decompression for queue pair %hu of compress device "
              "%hhu.\n",
              queue_pair_id, device_id);
    }
    async_success &= ret == arrow_dpdk::kAsyncReturnOK;

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  if (!async_success) {
    return arrow::Status::IOError("Failed to complete async decompression");
  }

  auto decompression_duration = std::chrono::duration_cast<std::chrono::microseconds>(
                                    decompression_end - decompression_start)
                                    .count();
  fmt::print("-> Duration: {:d} microseconds\t\tThroughput: {:.2f} Gbps\n",
             decompression_duration,
             num_parallel_tests() *
                 static_cast<double>(decompressed_buffer_vector.front()->size()) *
                 kBitsPerByte / kGigabit / static_cast<double>(decompression_duration) *
                 kMicroseconds);

  return arrow::Status::OK();
}

arrow::Status EvaluateSync(const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device,
                           const std::unique_ptr<arrow::Buffer>& input_buffer) {
  const std::uint16_t queue_pair_id = 0;

  fmt::print(
      "\n==================================================\n"
      "Sync Compress (on queue_pair_id: {:d})"
      "\n==================================================\n",
      queue_pair_id);

  arrow_dpdk::BufferVector compressed_buffers;

  for (int idx = 0; idx < kNumTests; ++idx) {
    ARROW_ASSIGN_OR_RAISE(compressed_buffers,
                          BenchmarkCompressSync(device, queue_pair_id, input_buffer));

    if (idx < kNumTests - 1) {
      // Keep the last compression result for the sync decompression test
      ARROW_RETURN_NOT_OK(device->Release(compressed_buffers));
      compressed_buffers.clear();
    } else {
      std::int64_t compressed_data_size = std::accumulate(
          compressed_buffers.begin(), compressed_buffers.end(), 0,
          [](std::int64_t size, const std::unique_ptr<arrow::Buffer>& buffer) {
            return size += buffer->size();
          });
      fmt::print("Compressed data size: {:d} bytes\n", compressed_data_size);
    }
  }

  fmt::print(
      "\n==================================================\n"
      "Sync Decompress (on queue_pair_id: {:d})"
      "\n==================================================\n",
      queue_pair_id);

  ARROW_ASSIGN_OR_RAISE_ELSE(
      auto decompressed_buffer,
      arrow::AllocateResizableBuffer(
          static_cast<std::int64_t>(compressed_buffers.size() * kDecompressedSegSize),
          arrow_dpdk::GetMemoryPool(arrow_dpdk::MemoryPoolBackend::Rtememzone)),
      ARROW_UNUSED(device->Release(compressed_buffers)));

  for (int idx = 0; idx < kNumTests; ++idx) {
    RETURN_NOT_OK_ELSE(BenchmarkDecompressSync(device, queue_pair_id, compressed_buffers,
                                               decompressed_buffer),
                       ARROW_UNUSED(device->Release(compressed_buffers)));
  }

  ARROW_RETURN_NOT_OK(device->Release(compressed_buffers));
  compressed_buffers.clear();

  if (decompressed_buffer->size() != input_buffer->size()) {
    return arrow::Status::Invalid(
        "Decompressed buffer length is not equal to the input buffer length");
  }
  if (std::memcmp(decompressed_buffer->data(), input_buffer->data(),
                  static_cast<std::size_t>(input_buffer->size())) != 0) {
    return arrow::Status::Invalid(
        "Decompressed buffer is not the same as the input buffer");
  }
  fmt::print("The decompressed buffer is equivalent to the input buffer\n");

  return arrow::Status::OK();
}

arrow::Status EvaluateAsync(
    const std::vector<std::unique_ptr<arrow_dpdk::MLX5CompressDevice>>& devices,
    const std::unique_ptr<arrow::Buffer>& input_buffer) {
  std::uint32_t total_num_qps =
      std::accumulate(devices.begin(), devices.end(), 0U,
                      [](std::uint32_t num,
                         const std::unique_ptr<arrow_dpdk::MLX5CompressDevice>& device) {
                        return num + device->num_qps();
                      });
  if (total_num_qps < num_parallel_tests()) {
    return arrow::Status::Cancelled("Total # of allocated queue pairs (", total_num_qps,
                                    ") < num_parallel_tests (", num_parallel_tests(),
                                    ")");
  }

  std::vector<std::string> device_to_qp;

  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];
    device_to_qp.emplace_back(
        fmt::format("[{:d}->{:d}]", device->device_id(), queue_pair_id));

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  fmt::print(
      "\n==================================================\n"
      "Async Compress (on [device_id -> queue_pair_id]: {})"
      "\n==================================================\n",
      fmt::join(device_to_qp, ", "));

  arrow_dpdk::BufferVector input_buffer_vector(num_parallel_tests());

  // Prepare num_parallel_tests() copies of the input_buffer
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    ARROW_ASSIGN_OR_RAISE(
        input_buffer_vector[idx],
        arrow::AllocateBuffer(
            input_buffer->size(),
            arrow_dpdk::GetMemoryPool(arrow_dpdk::MemoryPoolBackend::Rtememzone)));

    rte_memcpy(input_buffer_vector[idx]->mutable_data(), input_buffer->data(),
               static_cast<std::size_t>(input_buffer->size()));
  }

  std::unordered_map<std::uint8_t, std::vector<arrow_dpdk::BufferVector>>
      device_to_compressed_buffers_vector;

  device_id = 0;
  queue_pair_id = 0;
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];
    device_to_compressed_buffers_vector[device_id].emplace_back();
    Advance(device_id, queue_pair_id, device->num_qps());
  }

  for (int idx = 0; idx < kNumTests; ++idx) {
    ARROW_RETURN_NOT_OK(BenchmarkCompressAsync(devices, input_buffer_vector,
                                               device_to_compressed_buffers_vector));

    // Keep the last compression results for the async decompression test
    if (idx < kNumTests - 1) {
      ARROW_RETURN_NOT_OK(Release(devices, device_to_compressed_buffers_vector));
    }
  }
  input_buffer_vector.clear();

  fmt::print(
      "\n==================================================\n"
      "Async Decompress (on [device_id -> queue_pair_id]: {})"
      "\n==================================================\n",
      fmt::join(device_to_qp, ", "));

  std::vector<std::unique_ptr<arrow::ResizableBuffer>> decompressed_buffer_vector(
      num_parallel_tests());

  device_id = 0;
  queue_pair_id = 0;
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    ARROW_ASSIGN_OR_RAISE_ELSE(
        decompressed_buffer_vector[idx],
        arrow::AllocateResizableBuffer(
            static_cast<std::int64_t>(
                device_to_compressed_buffers_vector[device_id][queue_pair_id].size() *
                kDecompressedSegSize),
            arrow_dpdk::GetMemoryPool(arrow_dpdk::MemoryPoolBackend::Rtememzone)),
        ARROW_UNUSED(Release(devices, device_to_compressed_buffers_vector)));

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  for (int idx = 0; idx < kNumTests; ++idx) {
    RETURN_NOT_OK_ELSE(
        BenchmarkDecompressAsync(devices, device_to_compressed_buffers_vector,
                                 decompressed_buffer_vector),
        ARROW_UNUSED(Release(devices, device_to_compressed_buffers_vector)));
  }

  ARROW_RETURN_NOT_OK(Release(devices, device_to_compressed_buffers_vector));

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    if (decompressed_buffer_vector[idx]->size() != input_buffer->size()) {
      return arrow::Status::Invalid(
          "Decompressed buffer length from test {:d} is not equal to the input buffer "
          "length",
          idx);
    }

    if (std::memcmp(decompressed_buffer_vector[idx]->data(), input_buffer->data(),
                    static_cast<std::size_t>(input_buffer->size())) != 0) {
      return arrow::Status::Invalid(
          "Decompressed buffer from test {:d} is not the same as the input buffer", idx);
    }
  }
  fmt::print("All {:d} decompressed buffers {} equivalent to the input buffer\n",
             num_parallel_tests(), num_parallel_tests() > 1 ? "are" : "is");

  return arrow::Status::OK();
}

arrow::Status Evaluate(const std::unique_ptr<arrow::Buffer>& input_buffer) {
  ARROW_ASSIGN_OR_RAISE(
      auto bluefield_devices,
      GetBlueFieldCompressDevices(static_cast<std::uint64_t>(input_buffer->size())));

  if (num_parallel_tests() < bluefield_devices.size()) {
    return arrow::Status::Invalid("Require the # of lcores >= # of devices (",
                                  bluefield_devices.size(), ") + 1 for the test\n");
  }

  ARROW_RETURN_NOT_OK(EvaluateSync(bluefield_devices[0], input_buffer));
  ARROW_RETURN_NOT_OK(EvaluateAsync(bluefield_devices, input_buffer));
  return arrow::Status::OK();
}

void SignalHandler(int signal) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Exit by signal {}\n", sys_siglist[signal]);
}

int main(int argc, char* argv[]) {
  if (std::signal(SIGTERM, SignalHandler) == SIG_ERR) {
    RTE_LOG(CRIT, USER1, "Unable to install the signal handler for SIGTERM\n");
    std::quick_exit(EXIT_FAILURE);
  }
  if (std::signal(SIGINT, SignalHandler) == SIG_ERR) {
    RTE_LOG(CRIT, USER1, "Unable to install the signal handler for SIGINT\n");
    std::quick_exit(EXIT_FAILURE);
  }

  std::string program{*argv};

  auto ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Invalid EAL arguments with error {}\n",
                               rte_strerror(rte_errno));
  }
  argc -= ret;
  argv += ret;

  std::string file_path;
  std::int64_t num_bytes_to_read = 0;
  try {
    cxxopts::Options options(std::move(program),
                             "\ndemo_app - Demonstrating (de)compression with SmartNICs");
    options.add_options()("f,file", "The file to read from as the input",
                          cxxopts::value<std::string>())(
        "b,bytes", "The number of bytes to read from file",
        cxxopts::value<std::int64_t>())("h,help", "Print help");

    auto parse_result = options.parse(argc, argv);

    if (parse_result.count("help") != 0) {
      arrow_dpdk::CleanupAndExit(EXIT_SUCCESS, options.help().c_str());
    }

    if (parse_result.count("file") == 0) {
      arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Missing argument for '--file'\n");
    }
    file_path = parse_result["file"].as<std::string>();

    if (parse_result.count("bytes") == 0) {
      arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Missing argument for '--bytes'\n");
    }
    num_bytes_to_read = parse_result["bytes"].as<std::int64_t>();
  } catch (const cxxopts::OptionException& e) {
    arrow_dpdk::CleanupAndExit(EXIT_FAILURE, e.what());
  }

  auto read_buffer_result = ReadFileBuffer(file_path, num_bytes_to_read);
  if (!read_buffer_result.ok()) {
    arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Unable to read buffer from file. [{}]\n",
                               read_buffer_result.status().ToString());
  }
  auto input_buffer = std::move(read_buffer_result).ValueOrDie();

  auto status = Evaluate(input_buffer);
  if (!status.ok()) {
    arrow_dpdk::CleanupAndExit(EXIT_FAILURE, "Failed to evaluate. [{}]\n",
                               status.ToString());
  }

  arrow_dpdk::CleanupAndExit(EXIT_SUCCESS, "\nEverything is OK!\n");
}
