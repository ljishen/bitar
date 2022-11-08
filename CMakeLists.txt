# MIT License
#
# Copyright (c) 2022 University of California
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

cmake_minimum_required(VERSION 3.20)

# set a default CXX and C standard for the tools and targets that do not specify
# them. If commented, the latest supported standard for your compiler is
# automatically set.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake_modules")

execute_process(
  COMMAND uname --machine
  OUTPUT_VARIABLE machine_hardware_name
  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL LAST)
string(TOLOWER "${machine_hardware_name}" machine_hardware_name_lower)
if(machine_hardware_name_lower STREQUAL "x86_64")
  set(machine_hardware_name_lower "x64")
endif()

set(MACHINE_OPTIONAL_DIR
    "${CMAKE_CURRENT_LIST_DIR}/opt/${machine_hardware_name_lower}")
file(MAKE_DIRECTORY "${MACHINE_OPTIONAL_DIR}")
list(APPEND CMAKE_PROGRAM_PATH "${MACHINE_OPTIONAL_DIR}/bin")

include(FetchContent)
# Globally suppress the warning for not giving DOWNLOAD_EXTRACT_TIMESTAMP to the
# FetchContent_Declare() or ExternalProject_Add() command.
# https://cmake.org/cmake/help/v3.24/policy/CMP0135.html
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

# Save the cost of downloading external source for different configurations
# (e.g., release and debug)
option(BITAR_FETCHCONTENT_OVERWRITE_CONFIGURATION
       "Overwrite existing populated content with current configuration" ON)
if(BITAR_FETCHCONTENT_OVERWRITE_CONFIGURATION)
  # Need to be set after including the FetchContent module
  set(FETCHCONTENT_BASE_DIR "${MACHINE_OPTIONAL_DIR}/_deps")
endif()

# Add project_options v0.26.1
FetchContent_Declare(
  _project_options
  # Ensure anything that depends on the extracted contents will be rebuilt
  # whenever the URL changes
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://github.com/aminya/project_options/archive/refs/tags/v0.26.1.zip)
FetchContent_MakeAvailable(_project_options)
include("${_project_options_SOURCE_DIR}/Index.cmake")

set(VCPKG_ROOT
    "${MACHINE_OPTIONAL_DIR}/src/vcpkg"
    CACHE PATH "vcpkg installation prefix")
get_filename_component(_vcpkg_parent_dir "${VCPKG_ROOT}" DIRECTORY)
file(MAKE_DIRECTORY "${_vcpkg_parent_dir}")
# Use vcpkg for library management - should be called before defining project():
# https://github.com/microsoft/vcpkg/tree/2022.09.27#vcpkg-as-a-submodule. If
# updating vcpkg dependencies is required, use "-DENABLE_VCPKG_UPDATE:BOOL=ON"
run_vcpkg(VCPKG_DIR "${VCPKG_ROOT}" VCPKG_URL
          "https://github.com/microsoft/vcpkg.git" ENABLE_VCPKG_UPDATE)

# Environment variable VCPKG_FORCE_SYSTEM_BINARIES must be set on arm, s390x,
# and ppc64le platforms before running the vcpkg toolchain that is invoked by
# the project command. See
# https://github.com/microsoft/vcpkg-tool/blob/2022-10-17/src/vcpkg.cpp#L281-L290
# The variable CMAKE_HOST_SYSTEM_PROCESSOR cannot be used for checking the host
# system architecture, because it is set only after the project command.
if((machine_hardware_name_lower MATCHES "^arm"
    OR machine_hardware_name_lower MATCHES "^aarch64"
    OR machine_hardware_name_lower MATCHES "^s390x"
    OR machine_hardware_name_lower MATCHES "^ppc64"
   )
   AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows"
   AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(ENV{VCPKG_FORCE_SYSTEM_BINARIES} 1)
endif()

include(Utilities)

# The default system-installed DPDK will be at /usr/local
set(dpdk_ROOT
    ""
    CACHE PATH "DPDK installation prefix")
if("${dpdk_ROOT}" STREQUAL "")
  # Build DPDK using vcpkg when the installation prefix is not specified. This
  # is required to be set before project().
  list(APPEND VCPKG_MANIFEST_FEATURES "dpdk")
else()
  # Paths that are prepended to PKG_CONFIG_PATH have higher priority than
  # pkg-config's default library search paths:
  # https://linux.die.net/man/1/pkg-config
  prepend_path_to_env(
    PKG_CONFIG_PATH
    # The first path is for Debian-based OS, and the second path is for Red
    # Hat-based OS
    "${dpdk_ROOT}/lib/${machine_hardware_name}-linux-gnu/pkgconfig:${dpdk_ROOT}/lib64/pkgconfig"
  )
endif()
set(dpdk_minimum_required_version 21.11.0)

# Define the features of the project
include(Features)

# Set the project name and language
project(
  bitar
  VERSION 0.0.3
  DESCRIPTION
    "Bitar is a C++ library to simplify accessing hardware compression/decompression accelerators."
  HOMEPAGE_URL "https://github.com/ljishen/bitar"
  LANGUAGES CXX C)

set(old_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
# Ensure that the .pc files installed by vcpkg has the highest priority
if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES
                                   "^[Dd][Ee][Bb][Uu][Gg]$")
  prepend_path_to_env(
    PKG_CONFIG_PATH
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/pkgconfig")
else()
  prepend_path_to_env(
    PKG_CONFIG_PATH
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig")
endif()

option(BITAR_BUILD_ARROW
       "Force to build the Arrow library to be used as the dependency" OFF)
if(NOT BITAR_BUILD_ARROW)
  set(Arrow_ROOT
      ""
      CACHE PATH "Arrow library installation prefix")
endif()
set(arrow_minimum_required_version 10.0.0)

set(IWYU_ROOT
    "${MACHINE_OPTIONAL_DIR}"
    CACHE PATH "include-what-you-use installation prefix")
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

if(BITAR_BUILD_TESTS)
  set(OPT_ENABLE_CLANG_TIDY "ENABLE_CLANG_TIDY")
  set(OPT_ENABLE_COVERAGE "ENABLE_COVERAGE")
  set(OPT_ENABLE_CPPCHECK "ENABLE_CPPCHECK")

  if(NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(OPT_ENABLE_SANITIZER_ADDRESS "ENABLE_SANITIZER_ADDRESS")
    set(OPT_ENABLE_SANITIZER_UNDEFINED_BEHAVIOR
        "ENABLE_SANITIZER_UNDEFINED_BEHAVIOR")
    set(OPT_ENABLE_SANITIZER_LEAK "ENABLE_SANITIZER_LEAK")
  else()
    # or it is MSVC and has run vcvarsall
    string(FIND "$ENV{PATH}" "$ENV{VSINSTALLDIR}" index_of_vs_install_dir)
    if(MSVC AND "${index_of_vs_install_dir}" STREQUAL "-1")
      set(OPT_ENABLE_SANITIZER_ADDRESS "ENABLE_SANITIZER_ADDRESS")
    endif()
  endif()
  set(OPT_WARNINGS_AS_ERRORS OFF)

  add_subdirectory(test)
endif()

if(BITAR_BUILD_DOCS)
  set(OPT_ENABLE_DOXYGEN "ENABLE_DOXYGEN")
else()
  set(OPT_ENABLE_DOXYGEN OFF)
endif()

include("${_project_options_SOURCE_DIR}/src/DynamicProjectOptions.cmake")
# Initialize project_options variable related to this project. This overwrites
# `project_options` and sets `project_warnings`. This also accepts the same
# arguments as `project_options`.
dynamic_project_options(
  # Enable CMAKE_INTERPROCEDURAL_OPTIMIZATION
  ENABLE_INTERPROCEDURAL_OPTIMIZATION
  # Add flag -march=native
  ENABLE_NATIVE_OPTIMIZATION
  CPPCHECK_OPTIONS
  --enable=style,performance,warning,portability
  --inline-suppr
  # We cannot act on a bug/missing feature of cppcheck
  --suppress=internalAstError
  # if a file does not have an internalAstError, we get an unmatchedSuppression
  # error
  --suppress=unmatchedSuppression
  --inconclusive
  --suppressions-list=${CMAKE_CURRENT_LIST_DIR}/suppressions.txt)

if(DEFINED CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
  list(APPEND CMAKE_CXX_INCLUDE_WHAT_YOU_USE
       "-Xiwyu;--mapping_file=${CMAKE_CURRENT_LIST_DIR}/iwyu.imp;-w")
endif()

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
            # warn if a precompiled header is found in the search path but
            # cannot be used
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

add_subdirectory(src)

if(BITAR_BUILD_APPS)
  add_subdirectory(apps)
endif()
