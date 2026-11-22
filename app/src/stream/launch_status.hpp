// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/i18n.hpp"

#include <chrono>
#include <string_view>

namespace lunanx
{
inline int LaunchPulseIndex(std::chrono::steady_clock::duration elapsed)
{
    return static_cast<int>((std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 200) % 3);
}

inline int LaunchStage(std::string_view status)
{
    if (status == "WebRTC completed") return 3;
    if (status.starts_with("Requesting a Luna") || status.starts_with("Luna queue") ||
        status.starts_with("Connecting to Luna") || status.starts_with("WebRTC ")) return 2;
    return 1;
}

inline std::string LaunchStatusText(std::string_view message, int stage)
{
    constexpr std::string_view queue = "Luna queue position ";
    if (message.starts_with(queue)) return Tr("Position dans la file") + "  " + std::string(message.substr(queue.size()));
    if (stage >= 3) return Tr("Synchronisation de l’image…");
    if (stage == 2) return Tr("Connexion au serveur Luna…");
    return Tr("Préparation du flux…");
}
}
