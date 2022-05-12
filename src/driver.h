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

#pragma once

#include <arrow/result.h>
#include <arrow/util/macros.h>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "config.h"
#include "device.h"

namespace bitar {

template <typename Class,
          typename = internal::IsEnumConstant<internal::DriverClass, Class>>
class CompressDriver {
 public:
  ARROW_DISALLOW_COPY_AND_ASSIGN(CompressDriver);
  CompressDriver(CompressDriver&&) = delete;
  CompressDriver& operator=(CompressDriver&&) = delete;

  /// \brief Return the global CompressDriver intstance of the type.
  static CompressDriver<Class>* Instance();

  /// \brief Get Devices with selected device_ids
  /// \param[in] device_ids the device ids managed by the driver
  /// \return a list of Device objects
  arrow::Result<std::vector<std::unique_ptr<CompressDevice<Class>>>> GetDevices(
      const std::vector<std::uint8_t>& device_ids);

  /// \brief Get all available device ids managed by the type of driver on the current
  /// machine.
  arrow::Result<std::vector<std::uint8_t>> ListAvailableDeviceIds();

 protected:
  /// \brief Return the const driver name in uppercase.
  [[nodiscard]] static auto driver_name() noexcept;

 private:
  CompressDriver() = default;
  ~CompressDriver() = default;
};

}  // namespace bitar
