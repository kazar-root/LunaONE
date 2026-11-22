// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include "stream/audio/OpusRecovery.hpp"
#include "stream/audio/PcmBuffer.hpp"
#include "core/playback_preferences.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace lunaone::apple
{

class AudioOutput
{
  public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    void Submit(std::span<const std::uint8_t> packet, std::uint16_t sequence);
    std::uint64_t PlayedBuffers() const { return played_.load(); }
    std::uint64_t UnderrunBuffers() const { return buffer_.Underruns(); }
    std::uint64_t DroppedPackets() const { return buffer_.Dropped(); }
    std::uint64_t NonSilentPackets() const { return non_silent_.load(); }

  private:
    explicit AudioOutput(const lunanx::PlaybackPreferences& settings);
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kSamplesPerBuffer = 480 * kChannels;
    static void Recycle(void* context, AudioQueueRef, AudioQueueBufferRef buffer) noexcept;
    OSStatus EnqueueAudio(AudioQueueBufferRef buffer) noexcept;

    AudioQueueRef queue_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
    std::array<AudioQueueBufferRef, 3> buffers_ {};
    int volume_ = 100;
    lunanx::audio::PcmBuffer buffer_;
    std::atomic<std::uint64_t> played_ {0};
    std::atomic<std::uint64_t> non_silent_ {0};
    std::atomic<OSStatus> enqueue_error_ {noErr};
    std::atomic<bool> stopping_ {false};
    bool running_ = false;
    lunanx::audio::OpusRecovery recovery_;
    std::array<std::int16_t, 5760 * kChannels> pcm_ {};
};

} // namespace lunaone::apple
