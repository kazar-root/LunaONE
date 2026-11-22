// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace lunaone::windows
{
struct VideoFrame
{
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

class VideoDecoder
{
  public:
    using FrameReady = std::function<void(std::shared_ptr<const VideoFrame>)>;
    VideoDecoder(FrameReady ready, int fps);
    ~VideoDecoder();

    bool Submit(std::span<const std::uint8_t> bytes, std::uint32_t timestamp);
    bool Ready() const { return ready_.load(); }
    std::string Error() const;

  private:
    struct Packet
    {
        std::vector<std::uint8_t> bytes;
        std::uint32_t timestamp;
        std::chrono::steady_clock::time_point arrival;
        bool keyframe;
        bool reset;
    };

    void Decode();

    FrameReady frame_ready_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Packet> packets_;
    const std::size_t queue_limit_;
    std::size_t queued_bytes_ = 0;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    std::string error_;
    bool stopping_ = false;
    bool needs_keyframe_ = true;
    std::atomic_bool ready_ = false;
    std::thread worker_;
};
} // namespace lunaone::windows
