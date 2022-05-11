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

#include "util.h"

#include <fstream>
#include <iterator>
#include <string>

#include <arrow/result.h>
#include <arrow/status.h>
#include <rte_build_config.h>
#include <rte_lcore.h>

namespace bitar::internal {

arrow::Result<std::string> ReadFileContent(const std::string& path) {
  std::ifstream istrm(path, std::ios::in);
  if (!istrm.is_open()) {
    return arrow::Status::IOError("Unable to open file of path '", path, "'");
  }
  return std::string((std::istreambuf_iterator<char>(istrm)),
                     std::istreambuf_iterator<char>());
}

std::uint32_t GetNextLcore(std::uint32_t start_id, int skip_main, int wrap) {
  if (wrap != 0) {
    start_id %= RTE_MAX_LCORE;
  }

  while (start_id < RTE_MAX_LCORE) {
    if (rte_lcore_is_enabled(start_id) == 0 ||
        (skip_main != 0 && start_id == rte_get_main_lcore())) {
      start_id++;
      if (wrap != 0) {
        start_id %= RTE_MAX_LCORE;
      }
      continue;
    }
    break;
  }
  return start_id;
}

}  // namespace bitar::internal
