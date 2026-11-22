// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lunanx::ui
{
// Row-major 0/1 modules with a four-module quiet zone on each edge.
struct DeviceQr
{
    int size = 0;
    std::vector<std::uint8_t> dark;
};

DeviceQr MakeDeviceQr(const std::string& verification_url);
}
