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
#
# ~~~
# This config sets the following target in your project::
#   DPDK::dpdk - for linked as static library
# ~~~

find_package(PkgConfig REQUIRED)
if(${PKG_CONFIG_VERSION_STRING} VERSION_LESS "0.28")
  message(
    FATAL_ERROR
      "  pkg-config version 0.28 or greater is required for DPDK.\n"
      "  Upgrade pkg-config or use the PKG_CONFIG environment variable to set the path to a newer version of pkg-config. See\n"
      "    https://doc.dpdk.org/guides/linux_gsg/sys_reqs.html#compilation-of-the-dpdk"
  )
endif()
pkg_check_modules(DPDK REQUIRED libdpdk)
mark_as_advanced(DPDK_STATIC_INCLUDE_DIRS DPDK_STATIC_CFLAGS
                 DPDK_STATIC_LDFLAGS DPDK_VERSION)

add_library(DPDK::dpdk INTERFACE IMPORTED)
unset(DPDK_FOUND)

# https://bechsoftware.com/2021/12/05/configuring-dpdk-projects-with-cmake/
set_target_properties(DPDK::dpdk PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${DPDK_STATIC_INCLUDE_DIRS}")
target_compile_options(DPDK::dpdk INTERFACE ${DPDK_STATIC_CFLAGS})
target_link_libraries(DPDK::dpdk INTERFACE ${DPDK_STATIC_LDFLAGS})

find_package_handle_standard_args(
  dpdk
  REQUIRED_VARS DPDK_STATIC_INCLUDE_DIRS DPDK_STATIC_CFLAGS DPDK_STATIC_LDFLAGS
  VERSION_VAR DPDK_VERSION)
