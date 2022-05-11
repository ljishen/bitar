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

#include <cstdint>
#include <memory>
#include <string>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/macros.h>
#include <rte_launch.h>

#include "config.h"
#include "device.h"
#include "type_fwd.h"

namespace arrow {
class Buffer;
class ResizableBuffer;
}  // namespace arrow

namespace bitar {

static inline constexpr auto kAsyncReturnOK = 2;

template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
struct CompressParam {
  /// The signature of the result_callback should be equivalent to the following:
  /// ---
  /// int func(std::uint8_t device_id, std::uint16_t queue_pair_id,
  /// arrow::Result<bitar::BufferVector>&& result);
  /// ---
  /// The return integer of the result_callback can be captured by a later call of
  /// rte_eal_wait_lcore()
  ///
  /// Having this constructor allows make_unique to construct the object without using
  /// brace initialization.
  CompressParam(const std::unique_ptr<bitar::CompressDevice<Class>>& device,
                std::uint16_t queue_pair_id,
                const std::unique_ptr<arrow::Buffer>& decompressed_buffer,
                const Callback& result_callback)
      : device_{device},
        queue_pair_id_{queue_pair_id},
        decompressed_buffer_{decompressed_buffer},
        result_callback_{result_callback} {}

  const std::unique_ptr<bitar::CompressDevice<Class>>& device_;
  const std::uint16_t queue_pair_id_{};
  const std::unique_ptr<arrow::Buffer>& decompressed_buffer_;
  const Callback& result_callback_;
};

template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
struct DecompressParam {
  /// The signature of the result_callback should be equivalent to the following, although
  /// the `const &` is not strictly required:
  /// ---
  /// int func(std::uint8_t device_id, std::uint16_t queue_pair_id, const arrow::Status&
  /// status);
  /// ---
  /// The return integer of the result_callback can be captured by a later call of
  /// rte_eal_wait_lcore()
  DecompressParam(const std::unique_ptr<bitar::CompressDevice<Class>>& device,
                  std::uint16_t queue_pair_id, const BufferVector& compressed_buffers,
                  const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer,
                  const Callback& result_callback)
      : device_{device},
        queue_pair_id_{queue_pair_id},
        compressed_buffers_{compressed_buffers},
        decompressed_buffer_{decompressed_buffer},
        result_callback_{result_callback} {}

  const std::unique_ptr<bitar::CompressDevice<Class>>& device_;
  const std::uint16_t queue_pair_id_{};
  const BufferVector& compressed_buffers_;
  const std::unique_ptr<arrow::ResizableBuffer>& decompressed_buffer_;
  const Callback& result_callback_;
};

namespace internal {

/// \brief Read the entire content of the file at the path to a string.
/// \param[in] path the path of the file to read.
arrow::Result<std::string> ReadFileContent(const std::string& path);

/// \brief Get the next enabled lcore starting from \p start_id
/// \param[in] start_id the lcore ID to start searching from
///
/// This is a fixed version of rte_get_next_lcore()
std::uint32_t GetNextLcore(std::uint32_t start_id, int skip_main, int wrap);

template <class T, class U>
std::shared_ptr<T> checked_pointer_cast(std::shared_ptr<U> r) noexcept {
#ifdef NDEBUG
  return std::static_pointer_cast<T>(std::move(r));
#else
  return std::dynamic_pointer_cast<T>(std::move(r));
#endif
}

template <class T, class U>
std::unique_ptr<T> checked_pointer_cast(std::unique_ptr<U> r) noexcept {
#ifdef NDEBUG
  return std::unique_ptr<T>(static_cast<T*>(r.release()));
#else
  return std::unique_ptr<T>(dynamic_cast<T*>(r.release()));
#endif
}

template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
int LcoreCompressFunc(void* compress_param) {
  auto* param = static_cast<CompressParam<Class, Callback>*>(compress_param);
  auto&& result =
      param->device_->Compress(param->queue_pair_id_, param->decompressed_buffer_);
  return param->result_callback_(param->device_->device_id(), param->queue_pair_id_,
                                 std::move(result));
}

template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
int LcoreDecompressFunc(void* decompress_param) {
  auto* param = static_cast<DecompressParam<Class, Callback>*>(decompress_param);
  auto&& status = param->device_->Decompress(
      param->queue_pair_id_, param->compressed_buffers_, param->decompressed_buffer_);
  return param->result_callback_(param->device_->device_id(), param->queue_pair_id_,
                                 status);
}

}  // namespace internal

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define ARROW_ASSIGN_OR_RAISE_ELSE_IMPL(result_name, lhs, rexpr, else_) \
  auto&& result_name = (rexpr);                                         \
  RETURN_NOT_OK_ELSE((result_name).status(), else_);                    \
  lhs = std::move(result_name).ValueUnsafe();

#define ARROW_ASSIGN_OR_RAISE_ELSE(lhs, rexpr, else_) \
  ARROW_ASSIGN_OR_RAISE_ELSE_IMPL(                    \
      ARROW_ASSIGN_OR_RAISE_NAME(_error_or_value, __COUNTER__), lhs, rexpr, else_);

#define ARROW_ASSIGN_OR_RAISE_NEGATIVE_ELSE(lhs, rexpr, msg, else_)                \
  do {                                                                             \
    lhs = (rexpr);                                                                 \
    if (lhs < 0) {                                                                 \
      else_;                                                                       \
      return ::arrow::Status::FromArgs(static_cast<arrow::StatusCode>(-lhs), msg); \
    }                                                                              \
  } while (false)

#define ARROW_ASSIGN_OR_RAISE_NEGATIVE(lhs, rexpr, msg) \
  ARROW_ASSIGN_OR_RAISE_NEGATIVE_ELSE(lhs, rexpr, msg, (void)0)

#define ARROW_RETURN_NOT_STATUS_OK_ELSE_IMPL(code_name, rexpr, msg, else_) \
  do {                                                                     \
    auto&& code_name = (rexpr);                                            \
    if (ARROW_PREDICT_FALSE(code_name != ::arrow::StatusCode::OK)) {       \
      else_;                                                               \
      return ::arrow::Status::FromArgs(code_name, msg);                    \
    }                                                                      \
  } while (false)

#define ARROW_RETURN_NOT_STATUS_OK_ELSE(rexpr, msg, else_)                         \
  ARROW_RETURN_NOT_STATUS_OK_ELSE_IMPL(ARROW_CONCAT(_error_or_value, __COUNTER__), \
                                       rexpr, msg, else_);

/// \brief Propagate any non-successful Status to the caller
#define ARROW_RETURN_NOT_STATUS_OK(rexpr, msg) \
  ARROW_RETURN_NOT_STATUS_OK_ELSE(rexpr, msg, (void)0)

#define ARROW_RETURN_NEGATIVE_NOT_STATUS_OK_IMPL(code_name, rexpr)               \
  do {                                                                           \
    auto&& code_name = (rexpr);                                                  \
    if (ARROW_PREDICT_FALSE(code_name != ::arrow::StatusCode::OK)) {             \
      return -static_cast<std::underlying_type_t<arrow::StatusCode>>(code_name); \
    }                                                                            \
  } while (false)

#define ARROW_RETURN_NEGATIVE_NOT_STATUS_OK(rexpr)                                     \
  ARROW_RETURN_NEGATIVE_NOT_STATUS_OK_IMPL(ARROW_CONCAT(_error_or_value, __COUNTER__), \
                                           rexpr)

// NOLINTEND(cppcoreguidelines-macro-usage)

/// \brief Asynchronously call to the \ref CompressDevice<Class>::Compress
/// "CompressDevice<Class>::Compress()"
/// \return 0 if the compression is successfully started, or -EBUSY if the corresponding
/// lcore is not in a WAIT state.
///
/// This function will use the lcore that is associated with the queue_pair_id for
/// compression.
template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
int CompressAsync(const std::unique_ptr<CompressParam<Class, Callback>>& param) {
  return rte_eal_remote_launch(internal::LcoreCompressFunc<Class, Callback>, param.get(),
                               param->device_->LcoreOf(param->queue_pair_id_));
}

/// \brief Asynchronously call to the \ref CompressDevice<Class>::Decompress
/// "CompressDevice<Class>::Decompress()"
/// \return 0 if the decompression is successfully started, or -EBUSY if the corresponding
/// lcore is not in a WAIT state.
///
/// This function will use the lcore that is associated with the queue_pair_id for
/// decompression.
template <typename Class, typename Callback,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
int DecompressAsync(const std::unique_ptr<DecompressParam<Class, Callback>>& param) {
  return rte_eal_remote_launch(internal::LcoreDecompressFunc<Class, Callback>,
                               param.get(),
                               param->device_->LcoreOf(param->queue_pair_id_));
}

}  // namespace bitar
