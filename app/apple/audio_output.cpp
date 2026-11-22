// SPDX-License-Identifier: GPL-3.0-only

#include "audio_output.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace lunaone::apple
{

AudioOutput::AudioOutput() : AudioOutput(lunanx::ResolvedPlaybackPreferences(lunanx::LoadPlaybackPreferences())) {}

AudioOutput::AudioOutput(const lunanx::PlaybackPreferences& settings) : buffer_(settings.buffer_ms)
{
    if (!settings.audio) return;
    volume_ = settings.volume;
    constexpr AudioStreamBasicDescription format {
        48000, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        4, 1, 4, 2, 16, 0};
    auto status = AudioQueueNewOutput(&format, Recycle, this, nullptr, nullptr, 0, &queue_);
    for (auto& buffer : buffers_)
    {
        if (status != noErr)
            break;
        status = AudioQueueAllocateBuffer(queue_, kSamplesPerBuffer * sizeof(std::int16_t), &buffer);
    }
    if (status == noErr)
        decoder_ = opus_decoder_create(48000, 2, nullptr);
    if (!decoder_)
    {
        if (queue_)
            AudioQueueDispose(queue_, true);
        throw std::runtime_error("Could not initialize audio output (" + std::to_string(status) + ").");
    }
}

AudioOutput::~AudioOutput()
{
    stopping_.store(true);
    if (queue_) AudioQueueDispose(queue_, true);
    if (decoder_) opus_decoder_destroy(decoder_);
}

void AudioOutput::Submit(std::span<const std::uint8_t> packet, std::uint16_t sequence)
{
    if (!queue_) return;
    if (const auto error = enqueue_error_.load(); error != noErr)
        throw std::runtime_error("Audio output failed (" + std::to_string(error) + ").");
    const int frames = recovery_.Decode(decoder_, packet, sequence, pcm_);
    if (frames <= 0)
        return;
    const std::size_t count = static_cast<std::size_t>(frames) * kChannels;
    lunanx::audio::ApplyVolume({pcm_.data(), count}, volume_);
    if (std::any_of(pcm_.data(), pcm_.data() + count, [](std::int16_t sample) { return sample < -4 || sample > 4; }))
        non_silent_.fetch_add(1, std::memory_order_relaxed);
    const auto queued = buffer_.Submit({pcm_.data(), count});
    if (!running_ && queued >= buffer_.Target())
    {
        for (auto buffer : buffers_)
            if (const auto error = EnqueueAudio(buffer); error != noErr)
                throw std::runtime_error("Could not queue audio (" + std::to_string(error) + ").");
        const auto result = AudioQueueStart(queue_, nullptr);
        if (result != noErr)
            throw std::runtime_error("Could not start audio output (" + std::to_string(result) + ").");
        running_ = true;
    }
}

OSStatus AudioOutput::EnqueueAudio(AudioQueueBufferRef buffer) noexcept
{
    auto* output = static_cast<std::int16_t*>(buffer->mAudioData);
    const auto count = buffer_.Render({output, kSamplesPerBuffer});
    buffer->mAudioDataByteSize = kSamplesPerBuffer * sizeof(std::int16_t);
    buffer->mUserData = count > 0 ? this : nullptr;
    return AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
}

void AudioOutput::Recycle(void* context, AudioQueueRef, AudioQueueBufferRef buffer) noexcept
{
    auto& output = *static_cast<AudioOutput*>(context);
    if (buffer->mUserData)
        output.played_.fetch_add(1, std::memory_order_relaxed);
    if (!output.stopping_.load())
        if (const auto error = output.EnqueueAudio(buffer); error != noErr)
            output.enqueue_error_.store(error);
}

} // namespace lunaone::apple
