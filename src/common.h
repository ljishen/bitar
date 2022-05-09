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

#include <fmt/core.h>
#include <rte_common.h>

namespace celium {

/// \brief A simple wrapper for the DPDK rte_exit().
///
/// This helps to avoid the clang-tidy warning of cppcoreguidelines-pro-type-vararg.
template <typename... T>
void CleanupAndExit(int exit_code, fmt::format_string<T...> fmt, T&&... args) {
  // This function will internally call rte_eal_cleanup() to release resources.
  rte_exit(exit_code, "%s", fmt::vformat(fmt, fmt::make_format_args(args...)).c_str());
}

void CleanupAndExit(int exit_code, const char* msg);

}  // namespace celium
