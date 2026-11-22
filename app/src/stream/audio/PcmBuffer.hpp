// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AudioLatencyPolicy.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>

namespace lunanx::audio
{
// One PCM producer and one device callback. Keep a single playout clock
// through gaps. Repeated underruns increase headroom temporarily. Latency
// is trimmed only when the buffer stayed above its target for a second.
class PcmBuffer
{
  public:
    explicit PcmBuffer(int milliseconds)
        : initial_ms_(ClampTargetBufferMs(milliseconds)), target_ms_(initial_ms_) {}

    std::size_t Submit(std::span<const std::int16_t> input)
    {
        if (input.empty()) return 0;
        const auto write = written_.load(std::memory_order_relaxed);
        const auto queued = write - read_.load(std::memory_order_acquire);
        if (input.size() > samples_.size() - queued)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return queued;
        }
        const auto offset = write % samples_.size();
        const auto first = std::min(input.size(), samples_.size() - offset);
        std::copy_n(input.data(), first, samples_.data() + offset);
        std::copy_n(input.data() + first, input.size() - first, samples_.data());
        written_.store(write + input.size(), std::memory_order_release);
        return queued + input.size();
    }

    std::size_t Render(std::span<std::int16_t> output)
    {
        if (output.empty()) return 0;
        auto read = read_.load(std::memory_order_relaxed);
        auto available = written_.load(std::memory_order_acquire) - read;
        const auto target = Target();
        if (!playing_)
        {
            // Startup waits for the whole target. After an underrun, half of it
            // is enough: waiting for all of it added that much silence to every
            // network hiccup, and repeated underruns raise the target anyway.
            const auto resume = underruns_.load(std::memory_order_relaxed)
                ? std::max<std::size_t>(target / 2, StereoSamples(10)) : target;
            playing_ = available >= std::max(resume, output.size());
        }

        // A burst released after a delivery stall is what carries playback
        // through the next stall. Trimming it at once turned each Wi-Fi hiccup
        // into a trim followed by an underrun, so only trim latency that the
        // lowest level of the last second shows to be persistent.
        std::size_t skipped = 0;
        if (playing_)
        {
            low_water_ = std::min(low_water_, available);
            window_samples_ += output.size();
            if (window_samples_ >= static_cast<std::size_t>(StereoSamples(1000)))
            {
                if (low_water_ > output.size() + target + StereoSamples(40))
                {
                    // Skip complete stereo frames on the consumer thread. Keep the
                    // old read cursor published until we have copied the output.
                    const auto keep = output.size() + target + StereoSamples(20);
                    skipped = std::min(low_water_ - keep, available - output.size()) & ~std::size_t{1};
                    read += skipped;
                    available -= skipped;
                    trimmed_frames_.fetch_add(static_cast<std::uint32_t>(skipped / 2), std::memory_order_relaxed);
                }
                ResetWindow();
            }
        }

        const auto count = playing_ ? std::min(available, output.size()) : 0;
        const bool underrun = playing_ && count < output.size();
        if (underrun)
        {
            playing_ = false;
            ResetWindow();
            const auto previous = underruns_.fetch_add(1, std::memory_order_relaxed);
            if (previous && stable_samples_ < StereoSamples(4000))
                target_ms_.store(std::min(TargetMs() + 20, 120), std::memory_order_relaxed);
            stable_samples_ = 0;
        }
        else if (playing_)
        {
            stable_samples_ += count;
            if (stable_samples_ >= StereoSamples(12000))
            {
                target_ms_.store(std::max(initial_ms_, TargetMs() - 10), std::memory_order_relaxed);
                stable_samples_ = 0;
            }
        }

        const auto offset = read % samples_.size();
        const auto first = std::min(count, samples_.size() - offset);
        std::copy_n(samples_.data() + offset, first, output.data());
        std::copy_n(samples_.data(), count - first, output.data() + first);
        std::fill(output.begin() + count, output.end(), 0);
        if (skipped)
            BlendStart(output.first(count));
        if (underrun)
            FadeEnd(output.first(count));
        if (output.size() >= 2)
            last_frame_ = {output[output.size() - 2], output.back()};
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    int TargetMs() const { return target_ms_.load(std::memory_order_relaxed); }
    std::size_t Target() const { return StereoSamples(TargetMs()); }
    std::uint32_t Underruns() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint32_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint32_t TrimmedFrames() const { return trimmed_frames_.load(std::memory_order_relaxed); }

  private:
    void ResetWindow()
    {
        low_water_ = std::numeric_limits<std::size_t>::max();
        window_samples_ = 0;
    }

    void BlendStart(std::span<std::int16_t> samples) const
    {
        const auto frames = static_cast<int>(std::min<std::size_t>(samples.size() / 2, 64));
        for (int frame = 0; frame < frames; ++frame)
            for (int channel = 0; channel < 2; ++channel)
            {
                auto& sample = samples[frame * 2 + channel];
                sample = static_cast<std::int16_t>(
                    (last_frame_[channel] * (frames - frame) + sample * frame) / frames);
            }
    }

    static void FadeEnd(std::span<std::int16_t> samples)
    {
        const auto frames = static_cast<int>(std::min<std::size_t>(samples.size() / 2, 64));
        auto tail = samples.last(frames * 2);
        for (int frame = 0; frame < frames; ++frame)
            for (int channel = 0; channel < 2; ++channel)
                tail[frame * 2 + channel] = static_cast<std::int16_t>(
                    tail[frame * 2 + channel] * (frames - frame - 1) / frames);
    }

    const int initial_ms_;
    std::atomic<int> target_ms_;
    std::array<std::int16_t, 32768> samples_{};
    std::atomic<std::size_t> read_{0}, written_{0};
    std::atomic<std::uint32_t> underruns_{0}, dropped_{0}, trimmed_frames_{0};
    std::size_t stable_samples_ = 0;
    // Lowest level seen before a render, over the last second of playback.
    std::size_t low_water_ = std::numeric_limits<std::size_t>::max();
    std::size_t window_samples_ = 0;
    std::array<std::int16_t, 2> last_frame_{};
    bool playing_ = false;
};
}
