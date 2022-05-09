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

#include "config.h"

#include <cstdint>
#include <limits>

#include <fmt/core.h>
#include <magic_enum.hpp>

namespace celium {

template <typename Class, typename Enable>
Configuration<Class, Enable>::Configuration() {
  UpdateCompressedSegSize();
}

template <typename Class, typename Enable>
std::string Configuration<Class, Enable>::ToString() const {
  return fmt::format(
      "burst_size: {:d}, max_sgl_segs: {:d}, decompressed_seg_size: {:d}, "
      "compressed_seg_size: {:d}, window_size: {:d}, huffman_enc: {}",
      burst_size_, max_sgl_segs_, decompressed_seg_size_, compressed_seg_size_,
      window_size_, magic_enum::enum_name(huffman_enc_));
}

template <typename Class, typename Enable>
rte_comp_xform Configuration<Class, Enable>::compress_xform() const noexcept {
  return rte_comp_xform{.type = RTE_COMP_COMPRESS};
}

template <typename Class, typename Enable>
rte_comp_xform Configuration<Class, Enable>::decompress_xform() const noexcept {
  return rte_comp_xform{.type = RTE_COMP_DECOMPRESS};
}

template <typename Class, typename Enable>
void Configuration<Class, Enable>::UpdateCompressedSegSize() noexcept {
  auto lower_bound = static_cast<std::uint32_t>(decompressed_seg_size_ << 1U);

  std::uint32_t num = std::numeric_limits<std::uint16_t>::max() + 1;
  while ((num & lower_bound) == 0) {
    num >>= 1U;
  }

  compressed_seg_size_ = static_cast<std::uint16_t>(
      num > (static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max() + 1) >>
             1U)
          ? (static_cast<double>(decompressed_seg_size_) * internal::kExpanseRatio)
          : num);
}

template class Configuration<Class_MLX5_PCI>;

std::string BlueFieldConfiguration::ToString() const {
  return fmt::format("{{ {}, checksum_type: {} }}",
                     Configuration<Class_MLX5_PCI>::ToString(),
                     magic_enum::enum_name(checksum_type_));
}

rte_comp_xform BlueFieldConfiguration::compress_xform() const noexcept {
  return rte_comp_xform{
      .type = RTE_COMP_COMPRESS,
      .compress = rte_comp_compress_xform{.algo = RTE_COMP_ALGO_DEFLATE,
                                          .deflate =
                                              rte_comp_deflate_params{
                                                  .huffman = huffman_enc(),
                                              },
                                          .level = 1,
                                          .window_size = window_size(),
                                          .chksum = checksum_type_,
                                          .hash_algo = RTE_COMP_HASH_ALGO_NONE}};
}

rte_comp_xform BlueFieldConfiguration::decompress_xform() const noexcept {
  rte_comp_xform xform{
      .type = RTE_COMP_DECOMPRESS,
      .decompress = rte_comp_decompress_xform{.algo = RTE_COMP_ALGO_DEFLATE,
                                              .chksum = checksum_type_,
                                              .window_size = window_size(),
                                              .hash_algo = RTE_COMP_HASH_ALGO_NONE}};
  // Bug in DPDK that mistakenly checks the hash_algo from the rte_comp_compress_xform
  // instead of from the rte_comp_decompress_xform. See
  // https://github.com/DPDK/dpdk/blob/v22.03/drivers/compress/mlx5/mlx5_compress.c#L318
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
  xform.compress.hash_algo = RTE_COMP_HASH_ALGO_NONE;
  return xform;
}

}  // namespace celium
