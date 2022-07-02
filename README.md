# bitar

[![GitHub Super-Linter](https://github.com/ljishen/bitar/workflows/Lint%20Code%20Base/badge.svg)](https://github.com/marketplace/actions/super-linter)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

<!-- markdownlint-disable-next-line no-inline-html -->
<img src="assets/logo.png" width="25%">
Bitar is a C++ library to simplify accessing hardware compression/decompression accelerators.

---

## Prerequisites

- Linux (with kernel >= 4.4) or FreeBSD
- For Linux, glibc >= 2.7 (reported by `ldd --version`)
- [DPDK](https://github.com/DPDK/dpdk) >= v21.11 (can be installed via vcpkg)
- [Apache Arrow](https://github.com/apache/arrow) >= 7.0.0 (can be installed via vcpkg)

## Supported Hardware

- [NVIDIA BLUEFIELD-2 DPU](https://www.nvidia.com/content/dam/en-zz/Solutions/Data-Center/documents/datasheet-nvidia-bluefield-2-dpu.pdf)

## Integration

Bitar can be easily installed and integrated via vcpkg

```bash
vcpkg install bitar
```

## Development

```bash
$ # Reserve hugepages
$ sudo sh -c 'echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages'
$ # On a NUMA machine, we need
$ # sudo sh -c 'echo 1024 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages'

$ # When DPDK or Arrow library is not found, it will be automatically built from source.
$ CC=clang CXX=clang++ cmake -S . -B ./build-$(uname -m) -G Ninja \
[-Ddpdk_ROOT:PATH=<dpdk-install-prefix>] [-DArrow_ROOT:PATH=<arrow-install-prefix>] \
-DBITAR_BUILD_APPS:BOOL=ON -DBITAR_BUILD_TESTS:BOOL=ON \
-DENABLE_DEVELOPER_MODE:BOOL=ON -DCMAKE_BUILD_TYPE:BOOL=Debug

$ cmake --build ./build-$(uname -m)
$ cmake --install ./build-$(uname -m) --prefix <install-prefix>

# LD_LIBRARY_PATH can be omitted if DPDK is built from source via vcpkg
$ LD_LIBRARY_PATH=<dpdk-install-prefix>/lib/$(uname -m)-linux-gnu:<dpdk-install-prefix>/lib64:$LD_LIBRARY_PATH \
./build-$(uname -m)/apps/demo_app --in-memory -l 1-3 -a <device-pci-id>,class=compress -- \
--file <file> --bytes <size-to-read-from-file>
```

### Advanced CMake Configuration Options

- `VCPKG_ROOT`: the prefix to an installed vcpkg instance (install automatically if not specified)
- `BITAR_BUILD_ARROW`: set this option to `ON` to force building the Arrow dependency from source (default: OFF)
- `BITAR_ARROW_GIT_REPOSITORY`: the git repository to fetch the Arrow source (default: the official repository)
- `BITAR_ARROW_GIT_TAG`: use the source at the git branch, tag or commit hash of the Arrow repository
- `BITAR_FETCHCONTENT_OVERWRITE_CONFIGURATION`: set this option to `OFF` to have separate debug and release builds without overwriting each others configurations (default: ON)
