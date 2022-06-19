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

if("${DPDK_ROOT}" STREQUAL "")
  # This is the case where DPDK is installed with vcpkg.
  find_package(dpdk REQUIRED CONFIG)
else()
  # This is the case where the installation prefix of DPDK is specified by the user.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

  add_library(DPDK::dpdk ALIAS PkgConfig::DPDK)

  find_package_handle_standard_args(
    dpdk
    REQUIRED_VARS DPDK_LIBRARIES DPDK_INCLUDE_DIRS
    VERSION_VAR DPDK_VERSION)
endif()
