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
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include <arrow/util/string_builder.h>
#include <rte_comp.h>
#include <rte_config.h>
#include <magic_enum.hpp>

namespace celium {

namespace internal {

static inline constexpr auto kExpanseRatio = 1.1;
static inline constexpr auto kMaxMbufDataSize =
    std::numeric_limits<std::uint16_t>::max() - RTE_PKTMBUF_HEADROOM;

static inline constexpr auto kMinSegSize = 8;
static inline constexpr auto kMaxSegSize =
    static_cast<std::uint16_t>(kMaxMbufDataSize / kExpanseRatio);
static inline constexpr auto kDefaultSegSize = 2048;

enum class DriverClass : std::int8_t { MLX5_PCI };

template <typename EnumClass, typename Constant>
using IsEnumConstant =
    std::enable_if_t<magic_enum::is_scoped_enum<EnumClass>::value &&
                     std::is_same_v<decltype(Constant::value), const EnumClass>>;

}  // namespace internal

using Class_MLX5_PCI =
    std::integral_constant<internal::DriverClass, internal::DriverClass::MLX5_PCI>;

template <typename Class,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
class Configuration : public arrow::util::ToStringOstreamable<Configuration<Class>> {
 public:
  Configuration();
  Configuration(const Configuration&) = default;
  Configuration(Configuration&&) noexcept = default;
  Configuration& operator=(const Configuration&) = default;
  Configuration& operator=(Configuration&&) noexcept = default;

  /// \brief Return human-readable representation of Configuration
  [[nodiscard]] virtual std::string ToString() const = 0;

  /// \brief The name identifying the kind of file format
  [[nodiscard]] virtual std::string_view type_name() const noexcept = 0;

  /// \brief Get the rte_comp_xform for compression operation.
  [[nodiscard]] virtual rte_comp_xform compress_xform() const noexcept = 0;

  /// \brief Get the rte_comp_xform for decompression operation.
  [[nodiscard]] virtual rte_comp_xform decompress_xform() const noexcept = 0;

  /// \brief Return the maximum number of chained mbufs (aka compression operations)
  /// enqueue/dequeue from the compress device in a single request.
  [[nodiscard]] auto burst_size() const noexcept { return burst_size_; }

  void set_burst_size(std::uint16_t burst_size) { burst_size_ = burst_size; }

  /// \brief Return the maximum number of segments chaining to an mbuf
  ///        assembling a scatter-gather list (SGL).
  ///
  /// Setting this parameter to 1 can disable chained mbufs.
  [[nodiscard]] auto max_sgl_segs() const noexcept { return max_sgl_segs_; }

  void set_max_sgl_segs(std::uint16_t max_sgl_segs) { max_sgl_segs_ = max_sgl_segs; }

  /// \brief Return the mbuf data length (aka chunk size) for storing decompressed data.
  [[nodiscard]] auto decompressed_seg_size() const noexcept {
    return decompressed_seg_size_;
  }

  void set_decompressed_seg_size(std::uint16_t decompressed_seg_size) {
    decompressed_seg_size_ = decompressed_seg_size;
    UpdateCompressedSegSize();
  }

  /// \brief Return the mbuf data length (aka chunk size) for storing compressed data.
  [[nodiscard]] auto compressed_seg_size() const noexcept { return compressed_seg_size_; }

  /// \brief Base two log value of sliding window to be used for generating data.
  [[nodiscard]] auto window_size() const noexcept { return window_size_; }

  void set_window_size(std::uint8_t window_size) { window_size_ = window_size; }

  /// \brief Compression huffman encoding type.
  [[nodiscard]] auto huffman_enc() const noexcept { return huffman_enc_; }

  void set_huffman_enc(rte_comp_huffman huffman_enc) { huffman_enc_ = huffman_enc; }

  /// \brief Return the max number of memzones to preallocate for this device.
  ///
  /// These memzones are used to store the compressed data output. Note that
  /// - the max number of memzones per process is RTE_MAX_MEMZONE, regardless of the
  ///   number of compress devices managed by the process
  /// - performance will be significantly impacted if the compressed data output uses more
  ///   memzones than the number of preallocated ones
  /// - performance will be slight impacted if the number of preallocated memzones is
  ///   orders of magnitude more than needed.
  [[nodiscard]] auto max_preallocate_memzones() const noexcept {
    return max_preallocate_memzones_;
  }

  void set_max_preallocate_memzones(std::uint16_t max_preallocate_memzones) {
    max_preallocate_memzones_ = max_preallocate_memzones;
  }

  virtual ~Configuration() = default;

 private:
  /// From performance point of view the compressed segment size should be a power of 2
  /// but also should be enough to store incompressible data. This function tries to find
  /// the nearest power of 2 buffer size that is greater than #decompressed_seg_size().
  void UpdateCompressedSegSize() noexcept;

  std::uint16_t burst_size_ = 32;
  std::uint16_t max_sgl_segs_ = 1U;
  std::uint16_t decompressed_seg_size_ = internal::kDefaultSegSize;
  std::uint16_t compressed_seg_size_{};
  std::uint8_t window_size_ = 0U;
  rte_comp_huffman huffman_enc_ = rte_comp_huffman::RTE_COMP_HUFFMAN_DYNAMIC;
  std::uint16_t max_preallocate_memzones_ = RTE_MAX_MEMZONE;
};

static inline constexpr std::string_view kBlueFieldConfigurationTypeName{"bluefield"};

/// \brief Specific configuration for BlueField DPU.
class BlueFieldConfiguration : public Configuration<Class_MLX5_PCI> {
 public:
  [[nodiscard]] std::string_view type_name() const noexcept override {
    return kBlueFieldConfigurationTypeName;
  }

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] rte_comp_xform compress_xform() const noexcept override;
  [[nodiscard]] rte_comp_xform decompress_xform() const noexcept override;

  /// \brief Type of checksum to generate on the input data.
  ///
  /// All supported types are: RTE_COMP_CHECKSUM_NONE (default), RTE_COMP_CHECKSUM_CRC32,
  /// RTE_COMP_CHECKSUM_ADLER32, and RTE_COMP_CHECKSUM_CRC32_ADLER32.
  [[nodiscard]] auto checksum_type() const noexcept { return checksum_type_; }

  void set_checksum_type(rte_comp_checksum_type checksum_type) {
    checksum_type_ = checksum_type;
  }

  static BlueFieldConfiguration Defaults() { return {}; }

 private:
  rte_comp_checksum_type checksum_type_ = rte_comp_checksum_type::RTE_COMP_CHECKSUM_NONE;
};

}  // namespace celium
