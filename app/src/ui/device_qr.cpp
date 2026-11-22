// SPDX-License-Identifier: GPL-3.0-only

#include "device_qr.hpp"

#include <qrcodegen.h>
#include <array>

namespace lunanx::ui
{
DeviceQr MakeDeviceQr(const std::string& verification_url)
{
    if (!verification_url.starts_with("https://") || verification_url.find('\0') != std::string::npos)
        return {};
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> work{}, code{};
    if (!qrcodegen_encodeText(verification_url.c_str(), work.data(), code.data(),
            qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return {};
    const int modules = qrcodegen_getSize(code.data());
    DeviceQr image{modules + 8, {}};
    image.dark.resize(static_cast<std::size_t>(image.size * image.size));
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            image.dark[static_cast<std::size_t>((y + 4) * image.size + x + 4)] = qrcodegen_getModule(code.data(), x, y);
    return image;
}
}
