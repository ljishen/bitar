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

add_library(celium "")
target_sources(
  celium
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

target_link_libraries(celium PRIVATE project_warnings project_options)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk>=21.11.0)

target_link_system_libraries(celium PUBLIC PkgConfig::DPDK)
target_compile_definitions(celium PUBLIC ALLOW_EXPERIMENTAL_API)

set(DEPENDENCIES_CONFIGURED magic_enum fmt CURL Arrow)
foreach(DEPENDENCY ${DEPENDENCIES_CONFIGURED})
  find_package(${DEPENDENCY} CONFIG REQUIRED)
endforeach()

if(TARGET arrow_static)
  list(APPEND ARROW_LIBRARIES arrow_static)
elseif(TARGET arrow_shared)
  list(APPEND ARROW_LIBRARIES arrow_shared)
endif()

target_link_system_libraries(celium PRIVATE magic_enum::magic_enum fmt::fmt
                             CURL::libcurl ${ARROW_LIBRARIES})

# Force the use of compile-time format string checks:
# https://fmt.dev/latest/api.html#compile-time-format-string-checks
target_compile_definitions(celium PUBLIC FMT_ENFORCE_COMPILE_STRING)
target_compile_definitions(celium PUBLIC ARROW_NO_DEPRECATED_API)

target_include_directories(celium PUBLIC ${CMAKE_CURRENT_LIST_DIR})

add_executable(demo_app demo_app.cc)
target_link_libraries(demo_app PRIVATE celium project_warnings project_options)
find_package(cxxopts CONFIG REQUIRED)
target_link_system_libraries(demo_app PRIVATE fmt::fmt ${ARROW_LIBRARIES}
                             cxxopts::cxxopts)

if("${ARROW_ROOT}" STREQUAL "")
  # This is the case when the arrow library is installed by vcpkg. But this
  # library is not built by statically linking with its dependencies. So we need
  # to manually specify its dependencies for the application.
  set(ARROW_DEPENDENCIES_CONFIGURED lz4 unofficial-brotli)
  foreach(DEPENDENCY ${ARROW_DEPENDENCIES_CONFIGURED})
    find_package(${DEPENDENCY} CONFIG REQUIRED)
  endforeach()

  target_link_system_libraries(
    demo_app PRIVATE lz4::lz4 unofficial::brotli::brotlidec-static
    unofficial::brotli::brotlienc-static
    unofficial::brotli::brotlicommon-static)
endif()
