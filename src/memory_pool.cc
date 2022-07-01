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

#include "include/memory_pool.h"

#include <arrow/memory_pool.h>
#include <arrow/status.h>
#include <arrow/util/logging.h>
#include <fmt/format.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <magic_enum.hpp>

namespace bitar {

namespace {

constexpr std::uint32_t kAlignment = 64;
constexpr std::int64_t kDebugXorSuffix = -0x181fe80e0b464188LL;

// A static piece of memory for 0-size allocations, so as to return
// an aligned non-null pointer.  Note the correct value for DebugAllocator
// checks is hardcoded.
// NOLINTNEXTLINE
alignas(kAlignment) std::int64_t zero_size_area[1] = {kDebugXorSuffix};
// NOLINTNEXTLINE
std::uint8_t* const kZeroSizeArea = reinterpret_cast<std::uint8_t*>(&zero_size_area);

class RtemallocAllocator {
 public:
  static arrow::Status AllocateAligned(std::int64_t size, std::uint8_t** out) {
    if (size == 0) {
      *out = kZeroSizeArea;
      return arrow::Status::OK();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    *out = reinterpret_cast<std::uint8_t*>(
        rte_malloc(nullptr, static_cast<std::size_t>(size), kAlignment));
    if (*out == nullptr) {
      return arrow::Status::OutOfMemory("malloc of size ", size, " failed");
    }
    return arrow::Status::OK();
  }

  static arrow::Status ReallocateAligned(std::int64_t old_size, std::int64_t new_size,
                                         std::uint8_t** ptr) {
    std::uint8_t* previous_ptr = *ptr;
    if (previous_ptr == kZeroSizeArea) {
      ARROW_DCHECK_EQ(old_size, 0);
      return AllocateAligned(new_size, ptr);
    }
    if (new_size == 0) {
      DeallocateAligned(previous_ptr, old_size);
      *ptr = kZeroSizeArea;
      return arrow::Status::OK();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    *ptr = reinterpret_cast<std::uint8_t*>(
        rte_realloc(*ptr, static_cast<std::size_t>(new_size), kAlignment));
    if (*ptr == nullptr) {
      *ptr = previous_ptr;
      return arrow::Status::OutOfMemory("realloc of size ", new_size, " failed");
    }
    return arrow::Status::OK();
  }

  static void DeallocateAligned(std::uint8_t* ptr, std::int64_t size) {
    if (ptr == kZeroSizeArea) {
      ARROW_DCHECK_EQ(size, 0);
    } else {
      rte_free(ptr);
    }
  }

  // DPDK automatically manages unused memory
  static void ReleaseUnused() {}
};

}  // namespace

class RtememzoneAllocator {
 public:
  static arrow::Status AllocateAligned(std::int64_t size, std::uint8_t** out) {
    if (size == 0) {
      *out = kZeroSizeArea;
      return arrow::Status::OK();
    }

    fmt::format_int name(rte_rdtsc());
    const auto* memzone = rte_memzone_reserve_aligned(
        name.c_str(), static_cast<std::size_t>(size),
        static_cast<std::int32_t>(rte_socket_id()), RTE_MEMZONE_IOVA_CONTIG, kAlignment);
    if (memzone == nullptr) {
      return arrow::Status::OutOfMemory("Reserving memzone of size ", size,
                                        " failed. [Error ", rte_errno, ": ",
                                        rte_strerror(rte_errno), "]");
    }

    // NOLINTNEXTLINE
    *out = reinterpret_cast<std::uint8_t*>(memzone->addr);
    RtememzoneAllocatorTracker::Instance()->Emplace(*out, memzone);
    return arrow::Status::OK();
  }

  static arrow::Status ReallocateAligned(std::int64_t old_size, std::int64_t new_size,
                                         std::uint8_t** ptr) {
    std::uint8_t* previous_ptr = *ptr;
    if (previous_ptr == kZeroSizeArea) {
      ARROW_DCHECK_EQ(old_size, 0);
      return AllocateAligned(new_size, ptr);
    }
    if (new_size == 0) {
      DeallocateAligned(previous_ptr, old_size);
      *ptr = kZeroSizeArea;
      return arrow::Status::OK();
    }

    // Allocate new chunk
    std::uint8_t* out = nullptr;
    ARROW_RETURN_NOT_OK(AllocateAligned(new_size, &out));
    ARROW_DCHECK(out);

    // Copy contents and release old memory chunk
    rte_memcpy(out, *ptr, static_cast<std::size_t>(RTE_MIN(new_size, old_size)));
    RtememzoneAllocatorTracker::Instance()->Release(*ptr);
    *ptr = out;
    return arrow::Status::OK();
  }

  static void DeallocateAligned(std::uint8_t* ptr, std::int64_t size) {
    if (ptr == kZeroSizeArea) {
      ARROW_DCHECK_EQ(size, 0);
    } else {
      RtememzoneAllocatorTracker::Instance()->Release(ptr);
    }
  }

  // Memzones are contiguous physical memory. So there is no unused memory requiring to
  // release after freeing any memzones.
  static void ReleaseUnused() {}
};

#ifndef NDEBUG
static constexpr std::uint8_t kAllocPoison = 0xBC;
static constexpr std::uint8_t kReallocPoison = 0xBD;
static constexpr std::uint8_t kDeallocPoison = 0xBE;
#endif

template <typename Allocator>
class BaseMemoryPoolImpl : public arrow::MemoryPool {
 public:
  arrow::Status Allocate(std::int64_t size, std::uint8_t** out) override {
    if (size < 0) {
      return arrow::Status::Invalid("negative malloc size");
    }
    if (static_cast<std::uint64_t>(size) >= std::numeric_limits<std::size_t>::max()) {
      return arrow::Status::OutOfMemory("malloc size overflows size_t");
    }
    ARROW_RETURN_NOT_OK(Allocator::AllocateAligned(size, out));
#ifndef NDEBUG
    // Poison data
    if (size > 0) {
      ARROW_DCHECK_NE(*out, nullptr);
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
      (*out)[0] = kAllocPoison;
      (*out)[size - 1] = kAllocPoison;
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
#endif

    stats_.UpdateAllocatedBytes(size);
    return arrow::Status::OK();
  }

  arrow::Status Reallocate(std::int64_t old_size, std::int64_t new_size,
                           std::uint8_t** ptr) override {
    if (new_size < 0) {
      return arrow::Status::Invalid("negative realloc size");
    }
    if (static_cast<std::uint64_t>(new_size) >= std::numeric_limits<std::size_t>::max()) {
      return arrow::Status::OutOfMemory("realloc overflows size_t");
    }
    ARROW_RETURN_NOT_OK(Allocator::ReallocateAligned(old_size, new_size, ptr));
#ifndef NDEBUG
    // Poison data
    if (new_size > old_size) {
      ARROW_DCHECK_NE(*ptr, nullptr);
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
      (*ptr)[old_size] = kReallocPoison;
      (*ptr)[new_size - 1] = kReallocPoison;
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
#endif

    stats_.UpdateAllocatedBytes(new_size - old_size);
    return arrow::Status::OK();
  }

  void Free(std::uint8_t* buffer, std::int64_t size) override {
#ifndef NDEBUG
    // Poison data
    if (size > 0) {
      ARROW_DCHECK_NE(buffer, nullptr);
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      buffer[0] = kDeallocPoison;
      buffer[size - 1] = kDeallocPoison;
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
#endif
    Allocator::DeallocateAligned(buffer, size);

    stats_.UpdateAllocatedBytes(-size);
  }

  void ReleaseUnused() override { Allocator::ReleaseUnused(); }

  [[nodiscard]] std::int64_t bytes_allocated() const override {
    return stats_.bytes_allocated();
  }

  [[nodiscard]] std::int64_t max_memory() const override { return stats_.max_memory(); }

 private:
  arrow::internal::MemoryPoolStats stats_;
};

class RtemallocMemoryPool : public BaseMemoryPoolImpl<RtemallocAllocator> {
 public:
  [[nodiscard]] std::string backend_name() const override { return "rtemalloc"; }
};

class RtememzoneMemoryPool : public BaseMemoryPoolImpl<RtememzoneAllocator> {
 public:
  [[nodiscard]] std::string backend_name() const override { return "rtememzone"; }
};

const rte_memzone* RtememzoneAllocatorTracker::Of(
    const std::uint8_t* addr) const noexcept {
  auto search = allocated_memzones_.find(addr);
  if (search == allocated_memzones_.end()) {
    return nullptr;
  }
  return search->second;
}

RtememzoneAllocatorTracker* RtememzoneAllocatorTracker::Instance() {
  static RtememzoneAllocatorTracker instance;
  return &instance;
}

void RtememzoneAllocatorTracker::Release(const std::uint8_t* addr) {
  const std::lock_guard<std::mutex> lock(mutex_);

  auto search = allocated_memzones_.find(addr);
  if (search == allocated_memzones_.end()) {
    return;
  }

  rte_memzone_free(allocated_memzones_[addr]);
  allocated_memzones_.erase(search);
}

arrow::MemoryPool* GetMemoryPool(MemoryPoolBackend backend) {
  arrow::MemoryPool* out = nullptr;

  switch (backend) {
    case MemoryPoolBackend::System:
      out = arrow::system_memory_pool();
      return out;
#ifdef ARROW_JEMALLOC
    case MemoryPoolBackend::Jemalloc:
      // Ignore the return status because the definition of ARROW_JEMALLOC is checked
      ARROW_UNUSED(arrow::jemalloc_memory_pool(&out));
      return out;
#endif
#ifdef ARROW_MIMALLOC
    case MemoryPoolBackend::Mimalloc:
      // Ignore the return status because the definition of ARROW_MIMALLOC is checked
      ARROW_UNUSED(arrow::mimalloc_memory_pool(&out));
      return out;
#endif
    case MemoryPoolBackend::Rtemalloc:
      static RtemallocMemoryPool rtemalloc_pool;
      return &rtemalloc_pool;
    case MemoryPoolBackend::Rtememzone:
      static RtememzoneMemoryPool rtememzone_pool;
      return &rtememzone_pool;
    default:
      auto backend_name = std::string{magic_enum::enum_name(backend)};
      rte_panic("Internal error: unimplemented memory pool %s", backend_name.data());
  }
}

}  // namespace bitar
