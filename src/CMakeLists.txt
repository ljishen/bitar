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

set(PRIVATE_DEPENDENCIES Arrow)
find_package(${PRIVATE_DEPENDENCIES} REQUIRED)
target_link_system_libraries(${PROJECT_NAME} PRIVATE Arrow::arrow)
# Disable APIs that have been deprecated
# https://arrow.apache.org/docs/developers/cpp/building.html#deprecations-and-api-changes
target_compile_definitions(${PROJECT_NAME} PRIVATE ARROW_NO_DEPRECATED_API)

include(GNUInstallDirs) # Define GNU standard installation directories such as
                        # CMAKE_INSTALL_INCLUDEDIR

set(PUBLIC_INCLUDES "include")
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/${PUBLIC_INCLUDES}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}>")

get_target_property(public_headers ${PROJECT_NAME} SOURCES)
list(FILTER public_headers INCLUDE REGEX "${PUBLIC_INCLUDES}/.+\.h$")
set_target_properties(${PROJECT_NAME} PROPERTIES PUBLIC_HEADER
                                                 "${public_headers}")
install(TARGETS ${PROJECT_NAME}
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}")

package_project(
  TARGETS
  ${PROJECT_NAME}
  project_options
  project_warnings
  PUBLIC_DEPENDENCIES
  ${PUBLIC_DEPENDENCIES}
  PRIVATE_DEPENDENCIES_CONFIGURED
  ${PRIVATE_DEPENDENCIES_CONFIGURED}
  PRIVATE_DEPENDENCIES
  ${PRIVATE_DEPENDENCIES})
