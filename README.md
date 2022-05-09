# Celium

[![GitHub Super-Linter](https://github.com/ljishen/celium/workflows/Lint%20Code%20Base/badge.svg)](https://github.com/marketplace/actions/super-linter)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

<!-- markdownlint-disable-next-line no-inline-html -->
<img src="assets/logo.png" width="20%">
Celium is a libary for accessing hardware compression/decompression accelerators.

---

## Prerequisites

- [DPDK](https://github.com/DPDK/dpdk) >= v21.11

## Support Hardware

- [NVIDIA BLUEFIELD-2 DPU](https://www.nvidia.com/content/dam/en-zz/Solutions/Data-Center/documents/datasheet-nvidia-bluefield-2-dpu.pdf)

## Integration

[TODO]

## Development

```bash
$ CC=clang CXX=clang++ cmake -S . -B ./build-$(uname -m) -G Ninja -DDPDK_ROOT=<dpdk/install/prefix> \
-DENABLE_DEVELOPER_MODE:BOOL=ON

$ cmake --build ./build-$(uname -m)

$ LD_LIBRARY_PATH=<dpdk/install/prefix>/lib/$(uname -m)-linux-gnu:<dpdk/install/prefix>/lib64:\
$LD_LIBRARY_PATH ./build-$(uname -m)/src/demo_app -l 1-3 -a <device_pci_id>,class=compress -- \
--file <file> --bytes <size_to_read_from_file>
```
