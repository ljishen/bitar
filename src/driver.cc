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

#include "include/driver.h"

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/logging.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <rte_build_config.h>
#include <rte_compressdev.h>
#include <rte_config.h>
#include <rte_lcore.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <magic_enum.hpp>

#include "include/device.h"
#include "include/util.h"

namespace bitar {

namespace {

arrow::Status HasDeviceIds(const std::vector<std::uint8_t>& avail_dev_ids,
                           const std::vector<std::uint8_t>& device_ids) {
  std::vector<std::uint8_t> not_avail_dev_ids;
  for (const auto& device_id : device_ids) {
    bool exists = std::find(std::begin(avail_dev_ids), std::end(avail_dev_ids),
                            device_id) != std::end(avail_dev_ids);
    if (!exists) {
      not_avail_dev_ids.emplace_back(device_id);
    }
  }

  if (!not_avail_dev_ids.empty()) {
    return arrow::Status::Invalid("Not available device ids: ",
                                  fmt::format("{}", fmt::join(not_avail_dev_ids, ", ")));
  }

  return arrow::Status::OK();
}

template <typename T, typename = std::enable_if_t<magic_enum::is_scoped_enum_v<T>>>
arrow::Result<T> GetPCIId(const char* device_name) {
  std::string type_name;
  if (std::is_same_v<T, internal::PCIVendorId>) {
    type_name = "vendor";
  } else if (std::is_same_v<T, internal::PCIDeviceId>) {
    type_name = "device";
  } else {
    return arrow::Status::Invalid("Invalid Enum type for getting the PCI id");
  }

  ARROW_ASSIGN_OR_RAISE(auto pci_id_str, internal::ReadFileContent(fmt::format(
                                             "/sys/class/infiniband/{}/device/{}",
                                             device_name, type_name)));

  try {
    auto pci_id_opt = magic_enum::enum_cast<T>(
        static_cast<std::uint16_t>(std::stoi(pci_id_str, nullptr, 0)));
    return pci_id_opt.value();
  } catch (...) {
    return arrow::Status::NotImplemented("Compress device with ", type_name, "_id '",
                                         pci_id_str, "' is not supported.");
  }
}

arrow::Result<std::vector<std::unique_ptr<MLX5CompressDevice>>> CreateDevices(
    const std::vector<std::uint8_t>& device_ids,
    const std::vector<std::uint32_t>& worker_lcores) {
  // Map all worker_lcores to the number of devices as even as possible, but ensure that
  // each device gets at least one lcores.
  auto min_num_qps_per_dev =
      static_cast<std::uint16_t>(worker_lcores.size() / device_ids.size());
  auto worker_lcores_iter = worker_lcores.begin();
  auto remaining_lcores = worker_lcores.size() % device_ids.size();

  std::vector<std::unique_ptr<MLX5CompressDevice>> devices;
  devices.reserve(device_ids.size());

  auto* manager = DeviceManager::Instance();

  for (const auto& device_id : device_ids) {
    auto num_lcores = min_num_qps_per_dev + (remaining_lcores > 0 ? 1 : 0);

    const auto* device_name = rte_compressdev_name_get(device_id);
    if (device_name == nullptr) {
      return arrow::Status::UnknownError("Unable to get the device name from device id: ",
                                         device_id);
    }

    ARROW_ASSIGN_OR_RAISE(auto pci_vendor_id,
                          GetPCIId<internal::PCIVendorId>(device_name));
    ARROW_ASSIGN_OR_RAISE(auto pci_device_id,
                          GetPCIId<internal::PCIDeviceId>(device_name));

    ARROW_RETURN_NOT_OK(magic_enum::enum_switch<arrow::Status>(
        [&](auto pci_vendor_id_val) {
          return magic_enum::enum_switch<arrow::Status>(
              [&](auto pci_device_id_val) {
                using PCIVendorIdType =
                    std::integral_constant<internal::PCIVendorId, pci_vendor_id_val>;
                using PCIDeviceIdType =
                    std::integral_constant<internal::PCIDeviceId, pci_device_id_val>;
                ARROW_ASSIGN_OR_RAISE(
                    auto* device,
                    (manager->Create<PCIVendorIdType, PCIDeviceIdType, Class_MLX5_PCI>(
                        device_id,
                        std::vector<std::uint32_t>(worker_lcores_iter,
                                                   worker_lcores_iter + num_lcores))));
                devices.emplace_back(device);
                return arrow::Status::OK();
              },
              pci_device_id);
        },
        pci_vendor_id));

    worker_lcores_iter += num_lcores;
    if (remaining_lcores > 0) {
      --remaining_lcores;
    }
  }
  return devices;
}

}  // namespace

template <typename Class, typename Enable>
CompressDriver<Class>* CompressDriver<Class, Enable>::Instance() {
  static CompressDriver<Class> instance;
  return &instance;
}

template <typename Class, typename Enable>
auto CompressDriver<Class, Enable>::driver_name() noexcept {
  return magic_enum::enum_name(Class::value);
}

template <>
arrow::Result<std::vector<std::uint8_t>>
CompressDriver<Class_MLX5_PCI>::ListAvailableDeviceIds() {
  // Get the lowercase driver name
  auto name = std::string{driver_name()};
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char chr) { return std::tolower(chr); });

  std::array<std::uint8_t, RTE_COMPRESS_MAX_DEVS> avail_dev_ids{};
  auto num_avail_dev = rte_compressdev_devices_get(name.data(), avail_dev_ids.data(),
                                                   RTE_COMPRESS_MAX_DEVS);
  if (num_avail_dev == 0) {
    return arrow::Status::Invalid("No compress device is available with driver name: ",
                                  driver_name());
  }
  return std::vector<std::uint8_t>(std::begin(avail_dev_ids),
                                   std::begin(avail_dev_ids) + num_avail_dev);
}

template <>
arrow::Result<std::vector<std::unique_ptr<MLX5CompressDevice>>>
CompressDriver<Class_MLX5_PCI>::GetDevices(const std::vector<std::uint8_t>& device_ids) {
  ARROW_ASSIGN_OR_RAISE(auto avail_dev_ids, ListAvailableDeviceIds());
  ARROW_RETURN_NOT_OK(HasDeviceIds(avail_dev_ids, device_ids));

  // leave one as the main lcore for management
  auto num_worker_lcores = rte_lcore_count() - 1;
  if (num_worker_lcores == 0) {
    return arrow::Status::Invalid(
        "No enough worker lcores for setting up queue pairs for devices.");
  }

  if (device_ids.size() > num_worker_lcores) {
    return arrow::Status::Invalid("The number of devices to set up (", device_ids.size(),
                                  ") is greater than the number of available worker "
                                  "lcores (",
                                  num_worker_lcores, ").");
  }

  std::vector<std::uint32_t> worker_lcores;
  worker_lcores.reserve(num_worker_lcores);

  for (std::uint32_t lcore_id = internal::GetNextLcore(0, 1, 0); lcore_id < RTE_MAX_LCORE;
       lcore_id = internal::GetNextLcore(++lcore_id, 1, 0)) {
    worker_lcores.push_back(lcore_id);
  }

  ARROW_CHECK_EQ(num_worker_lcores, worker_lcores.size());

  return CreateDevices(device_ids, worker_lcores);
}

template class CompressDriver<Class_MLX5_PCI>;

}  // namespace bitar
