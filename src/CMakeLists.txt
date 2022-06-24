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

add_library(${PROJECT_NAME} "")
target_sources(
  ${PROJECT_NAME}
  PRIVATE include/common.h
          include/config.h
          include/device.h
          include/driver.h
          include/memory_pool.h
          include/memory.h
          include/type_fwd.h
          include/util.h
          common.cc
          config.cc
          device.cc
          driver.cc
          memory_pool.cc
          memory.cc
          util.cc)

target_link_libraries(${PROJECT_NAME} PRIVATE project_options project_warnings)

set(PUBLIC_DEPENDENCIES_CONFIGURED "Arrow 7.0.0")
foreach(DEPENDENCY ${PUBLIC_DEPENDENCIES_CONFIGURED})
  string(REPLACE " " ";" DEPENDENCY "${DEPENDENCY}")
  # Use QUIET to silence the possible dependencies looking for by
  # ArrowConfig.cmake. This is not a problem because if possible dependencies
  # are not found, arrow_bundled_dependencies will be used.
  find_package(${DEPENDENCY} QUIET REQUIRED CONFIG)
endforeach()

if(TARGET arrow_static)
  list(APPEND ARROW_LIBRARIES arrow_static)
elseif(TARGET arrow_shared)
  list(APPEND ARROW_LIBRARIES arrow_shared)
endif()

target_link_system_libraries(${PROJECT_NAME} PUBLIC ${ARROW_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC ARROW_NO_DEPRECATED_API)

if("${ARROW_ROOT}" STREQUAL "")
  # This is the case where the vcpkg-installed Arrow library is used. Since the
  # Arrow library depends but does not statically link the compression
  # libraries, we need to expose these compression libraries to be used by apps
  # that depend on us.
  set(INTERFACE_DEPENDENCIES_CONFIGURED Snappy lz4 unofficial-brotli zstd)
  foreach(DEPENDENCY ${INTERFACE_DEPENDENCIES_CONFIGURED})
    find_package(${DEPENDENCY} REQUIRED CONFIG)
  endforeach()

  set(INTERFACE_DEPENDENCIES BZip2 ZLIB)
  foreach(DEPENDENCY ${INTERFACE_DEPENDENCIES})
    find_package(${DEPENDENCY} REQUIRED)
  endforeach()

  if(TARGET zstd::libzstd_shared)
    list(APPEND ZSTD_LIBRARIES zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static)
    list(APPEND ZSTD_LIBRARIES zstd::libzstd_static)
  endif()

  target_link_system_libraries(
    ${PROJECT_NAME}
    INTERFACE
    Snappy::snappy
    lz4::lz4
    unofficial::brotli::brotlidec-static
    unofficial::brotli::brotlienc-static
    unofficial::brotli::brotlicommon-static
    ${ZSTD_LIBRARIES}
    BZip2::BZip2
    ZLIB::ZLIB)
else()
  # This is the case when the system-installed Arrow library is used.
  set(INTERFACE_DEPENDENCIES_CONFIGURED CURL)
  find_package(${INTERFACE_DEPENDENCIES_CONFIGURED} REQUIRED CONFIG)
  target_link_system_libraries(${PROJECT_NAME} INTERFACE CURL::libcurl)
endif()

set(PRIVATE_DEPENDENCIES_CONFIGURED fmt magic_enum)
foreach(DEPENDENCY ${PRIVATE_DEPENDENCIES_CONFIGURED})
  find_package(${DEPENDENCY} REQUIRED CONFIG)
endforeach()

target_link_system_libraries(${PROJECT_NAME} PRIVATE fmt::fmt
                             magic_enum::magic_enum)
# Force the use of compile-time format string checks:
# https://fmt.dev/latest/api.html#compile-time-format-string-checks
target_compile_definitions(${PROJECT_NAME} PRIVATE FMT_ENFORCE_COMPILE_STRING)

set(PUBLIC_DEPENDENCIES "dpdk 21.11.0")
foreach(DEPENDENCY ${PUBLIC_DEPENDENCIES})
  string(REPLACE " " ";" DEPENDENCY "${DEPENDENCY}")
  find_package(${DEPENDENCY} REQUIRED)
endforeach()

target_link_system_libraries(${PROJECT_NAME} PUBLIC DPDK::dpdk)
target_compile_definitions(${PROJECT_NAME} PUBLIC ALLOW_EXPERIMENTAL_API)

set(INCLUDE_DIR "include")
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/${INCLUDE_DIR}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

package_project(
  TARGETS
  ${PROJECT_NAME}
  project_options
  project_warnings
  PUBLIC_INCLUDES
  "${INCLUDE_DIR}"
  INTERFACE_DEPENDENCIES_CONFIGURED
  ${INTERFACE_DEPENDENCIES_CONFIGURED}
  PUBLIC_DEPENDENCIES_CONFIGURED
  ${PUBLIC_DEPENDENCIES_CONFIGURED}
  INTERFACE_DEPENDENCIES
  ${INTERFACE_DEPENDENCIES}
  PUBLIC_DEPENDENCIES
  ${PUBLIC_DEPENDENCIES}
  PRIVATE_DEPENDENCIES_CONFIGURED
  ${PRIVATE_DEPENDENCIES_CONFIGURED})
