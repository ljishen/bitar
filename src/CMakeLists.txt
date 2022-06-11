# MIT License
#
# Copyright (c) 2022 Jianshen Liu
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

add_library(bitar "")
target_sources(
  bitar
  PRIVATE common.cc
          common.h
          config.cc
          config.h
          device.cc
          device.h
          driver.cc
          driver.h
          memory_pool.cc
          memory_pool.h
          memory.cc
          memory.h
          type_fwd.h
          util.cc
          util.h)

target_link_libraries(bitar PRIVATE project_warnings project_options)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk>=21.11.0)

target_link_system_libraries(bitar PUBLIC PkgConfig::DPDK)
target_compile_definitions(bitar PUBLIC ALLOW_EXPERIMENTAL_API)

set(DEPENDENCIES_CONFIGURED magic_enum fmt Arrow)
foreach(DEPENDENCY ${DEPENDENCIES_CONFIGURED})
  find_package(${DEPENDENCY} QUIET REQUIRED CONFIG)
endforeach()

target_link_system_libraries(bitar PRIVATE magic_enum::magic_enum fmt::fmt)
# Force the use of compile-time format string checks:
# https://fmt.dev/latest/api.html#compile-time-format-string-checks
target_compile_definitions(bitar PRIVATE FMT_ENFORCE_COMPILE_STRING)

if(TARGET arrow_static)
  list(APPEND ARROW_LIBRARIES arrow_static)
elseif(TARGET arrow_shared)
  list(APPEND ARROW_LIBRARIES arrow_shared)
endif()

target_link_system_libraries(bitar PUBLIC ${ARROW_LIBRARIES})
target_compile_definitions(bitar PUBLIC ARROW_NO_DEPRECATED_API)

target_include_directories(bitar PUBLIC ${CMAKE_CURRENT_LIST_DIR})

if("${ARROW_ROOT}" STREQUAL "")
  # This is the case where the vcpkg-installed Arrow library is used. Since the
  # Arrow library depends but does not statically link the compression
  # libraries, we need to expose these compression libraries to be used by apps
  # that depend on us.
  set(ARROW_DEPENDENCIES_CONFIGURED zstd lz4 Snappy unofficial-brotli)
  foreach(DEPENDENCY ${ARROW_DEPENDENCIES_CONFIGURED})
    find_package(${DEPENDENCY} REQUIRED CONFIG)
  endforeach()

  set(ARROW_DEPENDENCIES BZip2 ZLIB)
  foreach(DEPENDENCY ${ARROW_DEPENDENCIES_CONFIGURED})
    find_package(${DEPENDENCY} REQUIRED)
  endforeach()

  if(TARGET zstd::libzstd_shared)
    list(APPEND ZSTD_LIBRARIES zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static)
    list(APPEND ZSTD_LIBRARIES zstd::libzstd_static)
  endif()

  target_link_system_libraries(
    bitar
    INTERFACE
    ${ZSTD_LIBRARIES}
    lz4::lz4
    Snappy::snappy
    BZip2::BZip2
    ZLIB::ZLIB
    unofficial::brotli::brotlidec-static
    unofficial::brotli::brotlienc-static
    unofficial::brotli::brotlicommon-static)
else()
  # This is the case when the system-installed Arrow library is used.
  find_package(CURL REQUIRED CONFIG)
  target_link_system_libraries(bitar INTERFACE CURL::libcurl)
endif()
