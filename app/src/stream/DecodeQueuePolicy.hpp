// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=f73c2f77115f46bff6eb
// Project origin: https://github.com/kazar-root/LunaNX (third-party notices remain applicable).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lunanx::video
{

// Keep roughly 80 ms of complete pictures. The extra frame absorbs short
// retransmission/decode bursts without building a latency-heavy backlog.
constexpr std::size_t MaximumQueuedAccessUnits(int frames_per_second)
{
    const int safe_fps = std::max(1, frames_per_second);
    return static_cast<std::size_t>(
        std::clamp((safe_fps + 11) / 12, 2, 6));
}

// The transport can recover up to 110 ms of video in one burst.
constexpr int MaximumBurstQueueDelayMs = 160;
constexpr std::size_t MaximumBurstQueueBytes = 8 * 1024 * 1024;
constexpr std::size_t MaximumBurstAccessUnits(int frames_per_second)
{
    return 2 + static_cast<std::size_t>((std::clamp(frames_per_second, 1, 120) * MaximumBurstQueueDelayMs + 999) / 1000);
}

inline bool IsH264Keyframe(std::span<const std::uint8_t> bytes)
{
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i)
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 && (bytes[i + 3] & 31) == 5)
            return true;
    return false;
}

constexpr std::size_t ReadyFrameQueueCapacity()
{
    return 3;
}

constexpr std::size_t StartupReadyFrameDepth()
{
    return 2;
}

// Wait only when the ready queue is empty. This adaptive micro-reserve catches
// a decoder completion racing the UI frame without stalling input/render for a
// large part of the refresh interval. Runtime prediction may shorten or skip it.
constexpr int FrameWaitBudgetMicroseconds(int frames_per_second)
{
    const int safe_fps = std::max(1, frames_per_second);
    return std::clamp((1000000 / safe_fps) * 2 / 5, 2500, 6500);
}

} // namespace lunanx::video
