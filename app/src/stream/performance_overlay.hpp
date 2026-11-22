// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "input/luna_input_encoder.hpp"
#include "stream/live_metrics.hpp"

#include <atomic>
#include <cstdio>
#include <string>

namespace lunanx
{
// Update() runs on the controller thread, the other members on the UI thread.
class PerformanceOverlay
{
  public:
    explicit PerformanceOverlay(bool visible = false) : visible_(visible) {}
    PerformanceOverlay(const PerformanceOverlay& other)
        : sampler_(other.sampler_), visible_(other.visible_.load()), chord_(other.chord_) {}
    PerformanceOverlay& operator=(const PerformanceOverlay& other)
    {
        sampler_ = other.sampler_;
        visible_.store(other.visible_.load());
        chord_ = other.chord_;
        return *this;
    }

    input::GamepadState Update(input::GamepadState gamepad)
    {
        constexpr auto sticks = input::kLeftThumb | input::kRightThumb;
        const auto down = gamepad.buttons & sticks;
        if (down == sticks && !chord_)
        {
            visible_.store(!visible_.load());
            chord_ = true;
        }
        if (!down)
            chord_ = false;
        if (chord_)
            gamepad.buttons &= static_cast<std::uint16_t>(~sticks);
        return gamepad;
    }

    void Sample(StreamCounters counters)
    {
        sampler_.Update(StreamRateSampler::Clock::now(), counters);
    }

    bool Visible() const { return visible_.load(); }

    std::string Format(const std::string& connection = {}, int battery_percent = -1, int decoded_height = 0) const
    {
        char rates[64];
        if (sampler_.ready())
            std::snprintf(rates, sizeof(rates), "%.0f FPS · %.1f Mbps", sampler_.fps(), sampler_.mbps());
        else
            std::snprintf(rates, sizeof(rates), "– FPS · – Mbps");
        std::string result = (decoded_height > 0 ? std::to_string(decoded_height) + "p · " : "") + std::string(rates);
        if (!connection.empty())
            result += " · " + connection;
        if (battery_percent >= 0 && battery_percent <= 100)
            result += " · " + std::to_string(battery_percent) + "%";
        return result;
    }

  private:
    StreamRateSampler sampler_;
    std::atomic<bool> visible_ {false};
    bool chord_ = false;
};
}
