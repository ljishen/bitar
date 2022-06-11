// MIT License
//
// Copyright (c) 2022 Jianshen Liu
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "app_common.h"

#include <arrow/status.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "common.h"
#include "config.h"
#include "device.h"
#include "driver.h"

namespace bitar::app {

namespace {

void SignalHandler(int signal) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  bitar::CleanupAndExit(EXIT_FAILURE, "Exit by signal {}\n", strsignal(signal));
}

}  // namespace

void InstallSignalHandler() {
  if (std::signal(SIGTERM, SignalHandler) == SIG_ERR) {
    RTE_LOG(CRIT, USER1, "Unable to install the signal handler for SIGTERM\n");
    std::quick_exit(EXIT_FAILURE);
  }
  if (std::signal(SIGINT, SignalHandler) == SIG_ERR) {
    RTE_LOG(CRIT, USER1, "Unable to install the signal handler for SIGINT\n");
    std::quick_exit(EXIT_FAILURE);
  }
}

std::uint32_t num_parallel_tests() {
  // Reserve one of the lcores as the main lcore
  static std::uint32_t kNumParallelTests = rte_lcore_count() - 1;
  return kNumParallelTests;
}

arrow::Result<std::vector<std::unique_ptr<bitar::MLX5CompressDevice>>>
GetBlueFieldCompressDevices(std::uint64_t max_buffer_size) {
  auto* driver = bitar::CompressDriver<bitar::Class_MLX5_PCI>::Instance();
  ARROW_ASSIGN_OR_RAISE(auto device_ids, driver->ListAvailableDeviceIds());

  RTE_LOG(INFO, USER1, "Found devices with MLX5 driver: [%s]\n",
          fmt::format("{}", fmt::join(device_ids, ", ")).c_str());

  ARROW_ASSIGN_OR_RAISE(auto devices, driver->GetDevices(device_ids));

  for (auto& device : devices) {
    if (dynamic_cast<bitar::BlueFieldCompressDevice*>(device.get()) == nullptr) {
      return arrow::Status::Invalid("Compress device ", device->device_id(),
                                    " is not a BlueField device");
    }

    auto bluefield_config = std::make_unique<bitar::BlueFieldConfiguration>(
        bitar::BlueFieldConfiguration::Defaults());
    bluefield_config->set_decompressed_seg_size(kDecompressedSegSize);
    bluefield_config->set_burst_size(kBurstSize);

    auto max_preallocate_memzones_total =
        (max_buffer_size + bluefield_config->decompressed_seg_size() - 1) /
        bluefield_config->decompressed_seg_size() * num_parallel_tests();
    auto max_preallocate_memzones = static_cast<std::uint16_t>(
        (max_preallocate_memzones_total + devices.size() - 1) / devices.size());
    bluefield_config->set_max_preallocate_memzones(max_preallocate_memzones);

    ARROW_RETURN_NOT_OK(device->Initialize(std::move(bluefield_config)));
  }

  return devices;
}

}  // namespace bitar::app
