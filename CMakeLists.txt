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

cmake_minimum_required(VERSION 3.19)

# set a default CXX and C standard for the tools and targets that do not specify
# them. If commented, the latest supported standard for your compiler is
# automatically set.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

# Add project_options v0.21.1
include(FetchContent)
FetchContent_Declare(
  _project_options
  URL https://github.com/aminya/project_options/archive/refs/tags/v0.21.1.zip)
FetchContent_MakeAvailable(_project_options)
include("${_project_options_SOURCE_DIR}/Index.cmake")

# CMAKE_HOST_SYSTEM_PROCESSOR work only after setting the project name with
# project(), but VCPKG_FORCE_SYSTEM_BINARIES needs to be set before project().
execute_process(
  COMMAND "uname" "--machine"
  OUTPUT_VARIABLE MACHINE_HARDWARE_NAME
  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL LAST)
string(TOLOWER "${MACHINE_HARDWARE_NAME}" MACHINE_HARDWARE_NAME_LOWER)

# vcpkg: Environment variable VCPKG_FORCE_SYSTEM_BINARIES must be set on arm,
# s390x, and ppc64le platforms. See
# https://github.com/microsoft/vcpkg-tool/blob/2022-02-24/src/vcpkg.cpp#L257-L266
if((MACHINE_HARDWARE_NAME_LOWER MATCHES "^arm"
    OR MACHINE_HARDWARE_NAME_LOWER MATCHES "^aarch64"
    OR MACHINE_HARDWARE_NAME_LOWER MATCHES "^s390x"
    OR MACHINE_HARDWARE_NAME_LOWER MATCHES "^ppc64"
   )
   AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows"
   AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(ENV{VCPKG_FORCE_SYSTEM_BINARIES} 1)
endif()

set(VCPKG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/opt/${MACHINE_HARDWARE_NAME_LOWER}/vcpkg"
    CACHE STRING "vcpkg installation prefix")
get_filename_component(_VCPKG_PARENT_DIR "${VCPKG_ROOT}" DIRECTORY)
file(MAKE_DIRECTORY "${_VCPKG_PARENT_DIR}")
# Use vcpkg for library management - should be called before defining project():
# https://github.com/microsoft/vcpkg/tree/2022.02.02#vcpkg-as-a-submodule. If
# updating vcpkg dependencies is required, use "-DENABLE_VCPKG_UPDATE:BOOL=ON"
run_vcpkg(VCPKG_DIR "${VCPKG_ROOT}" VCPKG_URL
          "https://github.com/ljishen/vcpkg.git")

set(ARROW_ROOT
    ""
    CACHE STRING "Apache Arrow installation prefix")
if("${ARROW_ROOT}" STREQUAL "")
  # Build the Arrow libraries using vcpkg if the installation prefix is not
  # specified. This is required to be set before project().
  list(APPEND VCPKG_MANIFEST_FEATURES "arrow")
endif()

# Set the project name and language
project(
  celium
  VERSION 0.0.1
  DESCRIPTION
    "Celium is a libary for accessing hardware compression/decompression accelerators"
  HOMEPAGE_URL "https://github.com/ljishen/celium"
  LANGUAGES CXX C)

include("${_project_options_SOURCE_DIR}/src/DynamicProjectOptions.cmake")

set(IWYU_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/opt/include-what-you-use/build-${MACHINE_HARDWARE_NAME_LOWER}"
    CACHE STRING "include-what-you-use installation prefix")
# Add the search path for finding include-what-you-use. This must be set before
# calling dynamic_project_options() or project_options()
list(APPEND CMAKE_PROGRAM_PATH "${IWYU_ROOT}/bin")

# ~~~
# Any default can be overridden
# set(<feature_name>_DEFAULT <value>) - set default for both user and developer modes
# set(<feature_name>_DEVELOPER_DEFAULT <value>) - set default for developer mode
# set(<feature_name>_USER_DEFAULT <value>) - set default for user mode
# ~~~

set(ENABLE_CLANG_TIDY_DEVELOPER_DEFAULT OFF)
set(ENABLE_CPPCHECK_DEVELOPER_DEFAULT OFF)
set(ENABLE_INCLUDE_WHAT_YOU_USE_DEVELOPER_DEFAULT ON)

# Initialize project_options variable related to this project. This overwrites
# `project_options` and sets `project_warnings`. This also accepts the same
# arguments as `project_options`.
dynamic_project_options(
  # Enable CMAKE_INTERPROCEDURAL_OPTIMIZATION and add flag -march=native
  ENABLE_INTERPROCEDURAL_OPTIMIZATION ENABLE_NATIVE_OPTIMIZATION)

# Add warnings used by DPDK
target_compile_options(
  project_warnings
  INTERFACE # warn whenever a pointer is cast so as to remove a type qualifier
            # from the target type
            -Wcast-qual
            # warn about usage of deprecated features
            -Wdeprecated
            # check the arguments for printf and scanf, etc., have types
            # appropriate to the format string specified
            -Wformat
            # warn if the format string is not a string literal and so cannot be
            # checked
            -Wformat-nonliteral
            # warn about uses of format functions that represent possible
            # security problems
            -Wformat-security
            # warn if a precompiled header is found in the search path but cannot be used
            -Winvalid-pch
            # warn if a global function is defined without a previous
            # declaration
            -Wmissing-declarations
            # warn if a global function is defined without a previous prototype
            # declaration
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
            # warn if an extern declaration is encountered within a function
            $<$<COMPILE_LANGUAGE:C>:-Wnested-externs>
            # warn if an old-style function definition is used
            $<$<COMPILE_LANGUAGE:C>:-Wold-style-definition>
            # warn about anything that depends on the "size of" a function type
            # or of void
            -Wpointer-arith
            # warn if a function is declared or defined without specifying the
            # argument types
            $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
            # warn if an undefined identifier is evaluated in an #if directive
            -Wundef
            # warn if trying to write into a string constant in C
            -Wwrite-strings
            # ==== Globally Disabled Warnings ====
            # do not warn when the address of packed member of struct or union
            # is taken
            -Wno-address-of-packed-member
            # do not want if a structure's initializer has some fields missing
            -Wno-missing-field-initializers
            # ~~~
            # do not warn if a structure field with explicitly specified
            # alignment in a packed struct or union is misaligned
            # -Wno-packed-not-aligned
            # do not warn about accesses to elements of zero-length array
            # members that might overlap other members of the same object
            # -Wno-zero-length-bounds
            # ~~~
)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Utilities.cmake")

set(DPDK_ROOT
    "/usr/local"
    CACHE STRING "DPDK installation prefix")
# Paths in PKG_CONFIG_PATH precedes the pkg-config default directory for
# searching DPDK libraries: https://linux.die.net/man/1/pkg-config
prepend_path_to_env(
  PKG_CONFIG_PATH
  "${DPDK_ROOT}/lib/${MACHINE_HARDWARE_NAME_LOWER}-linux-gnu/pkgconfig:${DPDK_ROOT}/lib64/pkgconfig"
)

add_subdirectory(src)

# Adding the tests:
option(ENABLE_TESTING "Enable the tests" ON)
if(ENABLE_TESTING)
  enable_testing()
  message(
    "Building Tests. Be sure to check out test/constexpr_tests for constexpr
testing")
  add_subdirectory(test)
endif()

option(ENABLE_FUZZING "Enable the fuzz tests" OFF)
if(ENABLE_FUZZING)
  message(
    "Building Fuzz Tests, using fuzzing sanitizer https://www.llvm.org/docs/LibFuzzer.html"
  )
  add_subdirectory(fuzz_test)
endif()

# package the project
# package_project(TARGETS celium)
