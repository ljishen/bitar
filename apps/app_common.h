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

#include <cstdint>
#include <memory>
#include <vector>

#include "device.h"

namespace bitar::app {

static inline constexpr auto kBitsPerByte = 8;
static inline constexpr auto kGigabit = 1e9;
static inline constexpr auto kMicroseconds = 1e6;

static inline constexpr auto kDecompressedSegSize = 59460;
static inline constexpr auto kBurstSize = 32 * 1;

void InstallSignalHandler();

std::uint32_t num_parallel_tests();

arrow::Result<std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>>
GetBlueFieldCompressDevices(std::uint64_t max_buffer_size);

}  // namespace bitar::app
