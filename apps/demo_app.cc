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

#include "demo_app.h"

#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/type_fwd.h>
#include <arrow/ipc/options.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>
#include <arrow/util/logging.h>
#include <arrow/util/macros.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_launch.h>
#include <rte_log.h>
#include <rte_memcpy.h>
#include <ext/alloc_traits.h>

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

#include <cxxopts.hpp>

#include "app_common.h"
#include "common.h"
#include "config.h"
#include "device.h"
#include "memory_pool.h"
#include "type_fwd.h"
#include "util.h"

namespace bitar::app {

namespace {

void PrintPerfNumbers(std::int64_t total_bytes, std::uint64_t start_tsc,
                      std::uint64_t end_tsc = rte_rdtsc_precise()) {
  auto duration =
      static_cast<double>(end_tsc - start_tsc) / static_cast<double>(rte_get_tsc_hz());
  fmt::print("-> Duration: {:.2f} microseconds\t\tThroughput: {:.2f} Gbps\n",
             duration * kMicroseconds,
             static_cast<double>(total_bytes) * kBitsPerByte / kGigabit / duration);
}

inline void Advance(std::uint8_t& device_id, std::uint16_t& queue_pair_id,
                    std::uint16_t num_qps) {
  ++queue_pair_id;
  if (queue_pair_id == num_qps) {
    queue_pair_id = 0;
    ++device_id;
  }
}

bool WaitForAsyncCompletion(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices) {
  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;
  bool async_success = true;

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    int ret = rte_eal_wait_lcore(device->LcoreOf(queue_pair_id));
    if (ret == 0) {
      RTE_LOG(ERR, USER1,
              "Unable to start async operation for queue pair %hu of compress device "
              "%hhu.\n",
              queue_pair_id, device_id);
    }
    async_success &= ret == bitar::kAsyncReturnOK;

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  return async_success;
}

void Recycle(const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
             const std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>&
                 device_to_compressed_buffers_vector) {
  for (const auto& [device_id, compressed_buffers_vector] :
       device_to_compressed_buffers_vector) {
    for (const auto& compressed_buffers : compressed_buffers_vector) {
      ARROW_CHECK_EQ(devices[device_id]->Recycle(compressed_buffers),
                     compressed_buffers.size())
          << "Recycled less than expected number of buffers";
    }
  }
}

}  // namespace

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
  write_options.memory_pool = bitar::GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone);

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
      arrow::AllocateBuffer(num_bytes,
                            bitar::GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone)));

  ARROW_ASSIGN_OR_RAISE(auto num_bytes_read,
                        file->Read(num_bytes, buffer->mutable_data()));
  if (num_bytes_read != num_bytes) {
    return arrow::Status::IOError("Unable to read ", num_bytes, " bytes from file");
  }

  return buffer;
}

arrow::Result<bitar::BufferVector> BenchmarkCompressSync(
    const std::unique_ptr<bitar::MLX5CompressDevice>& device, std::uint16_t queue_pair_id,
    const std::unique_ptr<arrow::Buffer>& decompressed_buffer) {
  auto start_tsc = rte_rdtsc_precise();

  ARROW_ASSIGN_OR_RAISE(auto compressed_buffers,
                        device->Compress(queue_pair_id, decompressed_buffer));

  PrintPerfNumbers(decompressed_buffer->size(), start_tsc);

  return compressed_buffers;
}

arrow::Status BenchmarkDecompressSync(
    const std::unique_ptr<bitar::MLX5CompressDevice>& device, std::uint16_t queue_pair_id,
    const bitar::BufferVector& compressed_buffers,
    const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer) {
  auto start_tsc = rte_rdtsc_precise();

  ARROW_RETURN_NOT_OK(
      device->Decompress(queue_pair_id, compressed_buffers, decompressed_buffer));

  PrintPerfNumbers(decompressed_buffer->size(), start_tsc);

  return arrow::Status::OK();
}

arrow::Status BenchmarkCompressAsync(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const bitar::BufferVector& input_buffer_vector,
    std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>&
        device_to_compressed_buffers_vector) {
  std::uint64_t end_tsc = 0;

  auto compress_result_callback =
      [&](std::uint8_t device_id, std::uint16_t queue_pair_id,
          arrow::Result<bitar::BufferVector>&& result) -> int {
    if (!result.ok()) {
      RTE_LOG(ERR, USER1,
              "Failed to complete async compression via queue pair %hu of compress "
              "device %hhu. [%s]\n",
              queue_pair_id, device_id, result.status().ToString().c_str());
      return EXIT_FAILURE;
    }
    end_tsc = rte_rdtsc_precise();

    device_to_compressed_buffers_vector.at(device_id)[queue_pair_id] =
        std::move(result).ValueUnsafe();
    return bitar::kAsyncReturnOK;
  };

  using CompressParamType =
      bitar::CompressParam<bitar::Class_MLX5_PCI, decltype(compress_result_callback)>;

  std::vector<std::unique_ptr<CompressParamType>> compress_param_vector;
  compress_param_vector.reserve(num_parallel_tests());
  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;

  auto start_tsc = rte_rdtsc_precise();

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    compress_param_vector.emplace_back(std::make_unique<CompressParamType>(
        device, queue_pair_id, input_buffer_vector[idx], compress_result_callback));
    if (bitar::CompressAsync(compress_param_vector[idx]) != 0) {
      break;
    }

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  bool async_success = WaitForAsyncCompletion(devices);

  // Avoid missing the opportunity to recycle by waiting till results from all worker
  // lcores are known
  if (!async_success) {
    Recycle(devices, device_to_compressed_buffers_vector);
    return arrow::Status::IOError("Failed to complete async compression");
  }

  PrintPerfNumbers(std::accumulate(input_buffer_vector.begin(), input_buffer_vector.end(),
                                   static_cast<std::int64_t>(0),
                                   [](std::int64_t acc, const auto& buffer) {
                                     return acc + buffer->size();
                                   }),
                   start_tsc, end_tsc);

  return arrow::Status::OK();
}

arrow::Status BenchmarkDecompressAsync(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>&
        device_to_compressed_buffers_vector,
    const std::vector<std::unique_ptr<arrow::ResizableBuffer>>&
        decompressed_buffer_vector) {
  std::uint64_t end_tsc = 0;

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
    end_tsc = rte_rdtsc_precise();

    return bitar::kAsyncReturnOK;
  };

  using DecompressParamType =
      bitar::DecompressParam<bitar::Class_MLX5_PCI, decltype(decompress_result_callback)>;

  std::vector<std::unique_ptr<DecompressParamType>> decompress_param_vector;
  decompress_param_vector.reserve(num_parallel_tests());
  std::uint8_t device_id = 0;
  std::uint16_t queue_pair_id = 0;

  auto start_tsc = rte_rdtsc_precise();

  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    const auto& device = devices[device_id];

    decompress_param_vector.emplace_back(std::make_unique<DecompressParamType>(
        device, queue_pair_id,
        device_to_compressed_buffers_vector.at(device_id)[queue_pair_id],
        decompressed_buffer_vector[idx], decompress_result_callback));
    if (bitar::DecompressAsync(decompress_param_vector[idx]) != 0) {
      break;
    }

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  bool async_success = WaitForAsyncCompletion(devices);

  if (!async_success) {
    return arrow::Status::IOError("Failed to complete async decompression");
  }

  PrintPerfNumbers(
      std::accumulate(
          decompressed_buffer_vector.begin(), decompressed_buffer_vector.end(),
          static_cast<std::int64_t>(0),
          [](std::int64_t acc, const auto& buffer) { return acc + buffer->size(); }),
      start_tsc, end_tsc);

  return arrow::Status::OK();
}

arrow::Status EvaluateSync(const std::unique_ptr<bitar::MLX5CompressDevice>& device,
                           const std::unique_ptr<arrow::Buffer>& input_buffer) {
  const std::uint16_t queue_pair_id = 0;

  fmt::print(
      "\n==================================================\n"
      "Sync Compress (on queue_pair_id: {:d})"
      "\n==================================================\n",
      queue_pair_id);

  bitar::BufferVector compressed_buffers;

  for (int idx = 0; idx < kNumTests; ++idx) {
    ARROW_ASSIGN_OR_RAISE(compressed_buffers,
                          BenchmarkCompressSync(device, queue_pair_id, input_buffer));

    if (idx < kNumTests - 1) {
      // Keep the last compression result for the sync decompression test
      ARROW_CHECK_EQ(device->Recycle(compressed_buffers), compressed_buffers.size())
          << "Recycled less than expected number of buffers";
      compressed_buffers.clear();
    } else {
      auto compressed_data_size = std::accumulate(
          compressed_buffers.begin(), compressed_buffers.end(),
          static_cast<std::int64_t>(0),
          [](std::int64_t size, const std::unique_ptr<arrow::Buffer>& buffer) {
            return size += buffer->size();
          });
      fmt::print("Sync compressed data size: {:d} bytes\n", compressed_data_size);
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
          bitar::GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone)),
      ARROW_UNUSED(device->Recycle(compressed_buffers)));

  for (int idx = 0; idx < kNumTests; ++idx) {
    RETURN_NOT_OK_ELSE(BenchmarkDecompressSync(device, queue_pair_id, compressed_buffers,
                                               decompressed_buffer),
                       ARROW_UNUSED(device->Recycle(compressed_buffers)));
  }

  ARROW_CHECK_EQ(device->Recycle(compressed_buffers), compressed_buffers.size())
      << "Recycled less than expected number of buffers";
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
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const std::unique_ptr<arrow::Buffer>& input_buffer) {
  auto total_num_qps = std::accumulate(
      devices.begin(), devices.end(), static_cast<std::uint32_t>(0),
      [](std::uint32_t num, const std::unique_ptr<bitar::MLX5CompressDevice>& device) {
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

  bitar::BufferVector input_buffer_vector(num_parallel_tests());

  // Split the input_buffer into num_parallel_tests() segments as even as possible
  auto min_segment_size = input_buffer->size() / num_parallel_tests();
  auto extra_bytes = input_buffer->size() % num_parallel_tests();
  auto input_buffer_shared =
      std::make_shared<arrow::Buffer>(input_buffer->data(), input_buffer->size());
  std::int64_t input_offset = 0;
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    auto segment_size = min_segment_size + (idx < extra_bytes ? 1 : 0);
    ARROW_ASSIGN_OR_RAISE(
        input_buffer_vector[idx],
        arrow::AllocateBuffer(
            segment_size, bitar::GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone)));

    rte_memcpy(input_buffer_vector[idx]->mutable_data(),
               arrow::SliceBuffer(input_buffer_shared, input_offset)->data(),
               static_cast<std::size_t>(segment_size));
    input_offset += segment_size;
  }

  std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>
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

    if (idx < kNumTests - 1) {
      // Keep the last compression results for the async decompression test
      Recycle(devices, device_to_compressed_buffers_vector);
    } else {
      std::int64_t compressed_data_size = 0;
      for (const auto& [_, compressed_buffers_vector] :
           device_to_compressed_buffers_vector) {
        for (const auto& compressed_buffers : compressed_buffers_vector) {
          compressed_data_size += std::accumulate(
              compressed_buffers.begin(), compressed_buffers.end(),
              static_cast<std::int64_t>(0), [](std::int64_t size, const auto& buffer) {
                return size + buffer->size();
              });
        }
      }

      fmt::print("Async compressed data size: {:d} bytes\n", compressed_data_size);
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
            bitar::GetMemoryPool(bitar::MemoryPoolBackend::Rtememzone)),
        Recycle(devices, device_to_compressed_buffers_vector));

    Advance(device_id, queue_pair_id, device->num_qps());
  }

  for (int idx = 0; idx < kNumTests; ++idx) {
    RETURN_NOT_OK_ELSE(
        BenchmarkDecompressAsync(devices, device_to_compressed_buffers_vector,
                                 decompressed_buffer_vector),
        Recycle(devices, device_to_compressed_buffers_vector));
  }

  Recycle(devices, device_to_compressed_buffers_vector);

  auto decompressed_buffer_size = std::accumulate(
      decompressed_buffer_vector.begin(), decompressed_buffer_vector.end(),
      static_cast<std::int64_t>(0),
      [](std::int64_t acc, const auto& buffer) { return acc + buffer->size(); });
  if (decompressed_buffer_size != input_buffer->size()) {
    return arrow::Status::Invalid(
        "Decompressed buffer length is not equal to the input buffer length");
  }

  input_offset = 0;
  for (std::uint32_t idx = 0; idx < num_parallel_tests(); ++idx) {
    if (std::memcmp(decompressed_buffer_vector[idx]->data(),
                    arrow::SliceBuffer(input_buffer_shared, input_offset)->data(),
                    static_cast<std::size_t>(decompressed_buffer_vector[idx]->size())) !=
        0) {
      return arrow::Status::Invalid(
          "Decompressed segment {:d} is not the same as the input buffer", idx);
    }
    input_offset += decompressed_buffer_vector[idx]->size();
  }

  fmt::print(
      "The aggregated decompressed data from {:d} segments is equivalent to the input "
      "buffer\n",
      num_parallel_tests());

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

}  // namespace bitar::app

int main(int argc, char* argv[]) {
  bitar::app::InstallSignalHandler();

  // We can't reduce the scope of the variable because argv may be modified afterwards.
  std::string program{*argv};  // cppcheck-suppress[variableScope]

  auto ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    bitar::CleanupAndExit(EXIT_FAILURE, "Invalid EAL arguments with error {}\n",
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
      bitar::CleanupAndExit(EXIT_SUCCESS, options.help().c_str());
    }

    if (parse_result.count("file") == 0) {
      bitar::CleanupAndExit(EXIT_FAILURE, "Missing argument for '--file'\n");
    }
    file_path = parse_result["file"].as<std::string>();

    if (parse_result.count("bytes") == 0) {
      bitar::CleanupAndExit(EXIT_FAILURE, "Missing argument for '--bytes'\n");
    }
    num_bytes_to_read = parse_result["bytes"].as<std::int64_t>();
  } catch (const cxxopts::OptionException& e) {
    bitar::CleanupAndExit(EXIT_FAILURE, e.what());
  }

  auto read_buffer_result = bitar::app::ReadFileBuffer(file_path, num_bytes_to_read);
  if (!read_buffer_result.ok()) {
    bitar::CleanupAndExit(EXIT_FAILURE, "Unable to read buffer from file. [{}]\n",
                          read_buffer_result.status().ToString());
  }
  auto input_buffer = std::move(read_buffer_result).ValueOrDie();

  auto status = bitar::app::Evaluate(input_buffer);
  if (!status.ok()) {
    bitar::CleanupAndExit(EXIT_FAILURE, "Failed to evaluate. [{}]\n", status.ToString());
  }

  bitar::CleanupAndExit(EXIT_SUCCESS, "\nEverything is OK!\n");
}
