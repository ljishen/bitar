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

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

struct rte_memzone;

namespace arrow {
class MemoryPool;
}  // namespace arrow

namespace celium {

class RtememzoneAllocatorTracker {
 public:
  /// \brief Look up the memzone associated with the virtual address \p addr
  /// \param[in] addr the virtual address
  /// \return a pointer to the memzone or nullptr if not found
  const rte_memzone* Of(const std::uint8_t* addr) const noexcept;

  [[nodiscard]] std::size_t count() const noexcept { return allocated_memzones_.size(); }

  static RtememzoneAllocatorTracker* Instance();

 private:
  auto Emplace(const std::uint8_t* addr, const rte_memzone* memzone) {
    return allocated_memzones_.emplace(addr, memzone);
  }

  /// \brief Release the memzone associated with the virtual address \p addr
  /// \param[in] addr the virtual address
  void Release(const std::uint8_t* addr);

  std::unordered_map<const std::uint8_t*, const rte_memzone*> allocated_memzones_{};

  std::mutex mutex_{};

  friend class RtememzoneAllocator;
};

enum class MemoryPoolBackend : std::uint8_t {
  System,
  Jemalloc,
  Mimalloc,
  Rtemalloc,
  Rtememzone
};

/// \brief Get the memory pool for the selected backend
arrow::MemoryPool* GetMemoryPool(MemoryPoolBackend backend);

}  // namespace celium
