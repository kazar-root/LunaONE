// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace lunanx::audio
{
// Hold only packets behind a gap. Contiguous audio takes the direct path.
class AudioReorderBuffer
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit AudioReorderBuffer(std::chrono::milliseconds hold) : hold_(hold) {}

    template <class Deliver>
    void Submit(std::span<const std::uint8_t> bytes, std::uint16_t sequence,
                Clock::time_point now, const Deliver& deliver,
                std::span<const std::uint8_t> redundant = {})
    {
        if (bytes.empty()) return;
        if (!initialized_)
        {
            initialized_ = true;
            expected_ = sequence;
        }
        auto distance = Distance(sequence);
        if (distance < 0) return;
        while (count_ && distance >= static_cast<int>(pending_.size()))
        {
            SkipGap(now, deliver);
            distance = Distance(sequence);
        }
        if (distance >= static_cast<int>(pending_.size()))
            expected_ = sequence;
        if (sequence == expected_)
        {
            deliver(bytes, sequence);
            ++expected_;
            Drain(now, deliver);
            return;
        }
        auto& slot = pending_[sequence % pending_.size()];
        if (!slot.bytes.empty()) return;
        slot.bytes.assign(bytes.begin(), bytes.end());
        slot.redundant.clear();
        if (!redundant.empty()) slot.redundant.assign(redundant.begin(), redundant.end());
        slot.sequence = sequence;
        slot.arrival = now;
        if (++count_ == 1) deadline_ = now + hold_;
    }

    template <class Deliver>
    void Tick(Clock::time_point now, const Deliver& deliver)
    {
        while (count_ && now >= deadline_) SkipGap(now, deliver);
    }

    std::uint32_t RedRepairs() const { return red_repairs_; }

  private:
    struct Packet
    {
        std::vector<std::uint8_t> bytes, redundant;
        std::uint16_t sequence = 0;
        Clock::time_point arrival{};
    };

    int Distance(std::uint16_t sequence) const { return static_cast<std::int16_t>(sequence - expected_); }

    template <class Deliver>
    void Drain(Clock::time_point now, const Deliver& deliver)
    {
        while (count_)
        {
            auto& slot = pending_[expected_ % pending_.size()];
            if (slot.bytes.empty()) break;
            deliver(std::span<const std::uint8_t>(slot.bytes), expected_++);
            slot.bytes.clear();
            --count_;
        }
        // Each cached packet has already spent time in the jitter buffer.
        // Do not add another full wait for every hole in the same burst.
        auto oldest = now;
        for (const auto& slot : pending_)
            if (!slot.bytes.empty()) oldest = std::min(oldest, slot.arrival);
        deadline_ = oldest + hold_;
    }

    template <class Deliver>
    void SkipGap(Clock::time_point now, const Deliver& deliver)
    {
        auto nearest = static_cast<int>(pending_.size());
        for (const auto& slot : pending_)
            if (!slot.bytes.empty()) nearest = std::min(nearest, Distance(slot.sequence));
        expected_ = static_cast<std::uint16_t>(expected_ + nearest);
        const auto& next = pending_[expected_ % pending_.size()];
        if (!next.redundant.empty())
        {
            deliver(std::span<const std::uint8_t>(next.redundant), static_cast<std::uint16_t>(expected_ - 1));
            ++red_repairs_;
        }
        Drain(now, deliver);
    }

    std::array<Packet, 16> pending_{};
    const std::chrono::milliseconds hold_;
    Clock::time_point deadline_{};
    std::size_t count_ = 0;
    std::uint16_t expected_ = 0;
    std::uint32_t red_repairs_ = 0;
    bool initialized_ = false;
};
}
