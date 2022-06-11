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
#include <arrow/type_fwd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "device.h"
#include "type_fwd.h"

namespace arrow {
class Buffer;
}
namespace arrow {
class ResizableBuffer;
}

namespace bitar::app {

static inline constexpr auto kNumTests = 3;

arrow::Result<arrow::BufferVector> ReadBuffers(const char* ipc_file_path);

arrow::Result<std::unique_ptr<arrow::Buffer>> ReadFileBuffer(
    const std::string& ipc_file_path, std::int64_t num_bytes);

arrow::Result<bitar::BufferVector> BenchmarkCompressSync(
    const std::unique_ptr<bitar::MLX5CompressDevice>& device, std::uint16_t queue_pair_id,
    const std::unique_ptr<arrow::Buffer>& decompressed_buffer);

arrow::Status BenchmarkDecompressSync(
    const std::unique_ptr<bitar::MLX5CompressDevice>& device, std::uint16_t queue_pair_id,
    const bitar::BufferVector& compressed_buffers,
    const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer);

arrow::Status BenchmarkCompressAsync(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const bitar::BufferVector& input_buffer_vector,
    std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>&
        device_to_compressed_buffers_vector);

arrow::Status BenchmarkDecompressAsync(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const std::unordered_map<std::uint8_t, std::vector<bitar::BufferVector>>&
        device_to_compressed_buffers_vector,
    const std::vector<std::unique_ptr<arrow::ResizableBuffer>>&
        decompressed_buffer_vector);

arrow::Status EvaluateSync(const std::unique_ptr<bitar::MLX5CompressDevice>& device,
                           const std::unique_ptr<arrow::Buffer>& input_buffer);

arrow::Status EvaluateAsync(
    const std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>& devices,
    const std::unique_ptr<arrow::Buffer>& input_buffer);

arrow::Status Evaluate(const std::unique_ptr<arrow::Buffer>& input_buffer);

}  // namespace bitar::app
