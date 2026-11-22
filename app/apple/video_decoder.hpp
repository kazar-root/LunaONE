// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace lunaone::apple
{

class VideoDecoder
{
  public:
    enum class Mode { Asynchronous, Synchronous };
    // Automatic and Hardware prefer the media engine; Hardware falls back to
    // Automatic when no hardware decoder accepts the stream. Software uses
    // Apple's CPU decoder.
    enum class Acceleration { Automatic, Hardware, Software };
    struct Counters
    {
        std::uint64_t awaiting_keyframe;
        std::uint64_t failures;
        std::uint64_t dropped;
        std::uint64_t restarts;
        OSStatus last_error;
    };

    explicit VideoDecoder(std::function<void()> frame_ready = {}, Mode mode = Mode::Asynchronous,
                          Acceleration acceleration = Acceleration::Automatic);
    ~VideoDecoder();
    bool Submit(std::span<const std::uint8_t> annex_b, std::uint32_t timestamp);
    void RequireKeyframe() { needs_idr_.store(true); }
    CVImageBufferRef TakeFrame(); // Transfers the retained reference to the caller.
    std::uint64_t DecodedFrames() const { return decoded_.load(); }
    bool HardwareActive() const { return hardware_.load(); }
    Counters Stats() const;

  private:
    static void Decoded(void* context, void*, OSStatus status, VTDecodeInfoFlags,
                        CVImageBufferRef image, CMTime, CMTime);
    bool CreateSession();
    bool CreateSession(bool allow_software, bool require_hardware);
    void Reset();
    void LogStream() const;
    static void LogPicture(CVImageBufferRef image);

    VTDecompressionSessionRef session_ = nullptr;
    CMVideoFormatDescriptionRef format_ = nullptr;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    std::mutex mutex_;
    CVImageBufferRef latest_ = nullptr;
    const std::function<void()> frame_ready_;
    const VTDecodeFrameFlags flags_;
    const Acceleration acceleration_;
    std::atomic<bool> picture_logged_ {false};
    std::atomic<unsigned> pending_ {0};
    std::atomic<std::uint64_t> decoded_ {0};
    std::atomic<std::uint64_t> awaiting_keyframe_ {0};
    std::atomic<std::uint64_t> failures_ {0};
    std::atomic<std::uint64_t> dropped_ {0};
    std::atomic<std::uint64_t> restarts_ {0};
    std::atomic<OSStatus> last_error_ {noErr};
    std::atomic<bool> hardware_ {false};
    std::atomic<bool> needs_idr_ {true};
    std::uint32_t first_timestamp_ = 0;
};

} // namespace lunaone::apple
