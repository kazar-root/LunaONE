// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=4a7aaa68fd82f6c67b9e
// Adapted for LunaCore on 2026-11-18.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "input/luna_input_encoder.hpp"
#include "core/playback_preferences.hpp"
#include "luna/luna_client.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

namespace lunanx
{

class LunaStreamSession
{
  public:
    struct Callbacks
    {
        std::function<bool(std::span<const std::uint8_t>, std::uint32_t)> video;
        std::function<void(std::span<const std::uint8_t>, std::uint16_t)> audio;
        std::function<void(const std::string&)> status;
    };

    struct Counters
    {
        std::uint64_t received_bytes;
        std::uint64_t video_units;
        std::uint64_t video_units_dropped;
        std::uint64_t video_rtp_packets;
        std::uint64_t video_ssrc_mismatches;
        std::uint64_t srtp_decrypt_failures;
        std::uint64_t audio_packets;
        std::uint64_t gamepad_reports;
        std::uint64_t gamepad_changes;
        int transport_rtt_ms;
        int transport_rtt_p95_ms;
    };

    LunaStreamSession(const luna::LunaClient& client, luna::Game game, Callbacks callbacks);
    ~LunaStreamSession();
    void Start();
    void Stop(bool end_remote_session = false);
    void SetGamepad(const input::GamepadState& state);
    Counters Stats() const;
    bool Finished() const { return finished_.load(); }
    std::string Error() const;

  private:
    struct Connection;
    void Run();
    void Stream(Connection& connection);
    bool Wait(std::chrono::milliseconds delay) const;
    void Report(const std::string& status) const;

    const luna::LunaClient& client_;
    luna::Game game_;
    Callbacks callbacks_;
    PlaybackPreferences preferences_;
    input::DisplayRequest display_;
    // Constrained Baseline offered on 98 behind Main or High.
    bool offered_fallback_ = false;
    std::thread worker_;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> finished_ {false};
    mutable std::mutex mutex_;
    std::string error_;
    std::string pending_job_id_;
    std::string session_id_;
    input::GamepadState gamepad_;
    int face_samples_ = 0;
    std::atomic<std::uint64_t> received_bytes_ {0};
    std::atomic<std::uint64_t> video_units_ {0};
    std::atomic<std::uint64_t> video_units_dropped_ {0};
    std::atomic<std::uint64_t> video_rtp_packets_ {0};
    std::atomic<std::uint64_t> video_ssrc_mismatches_ {0};
    std::atomic<std::uint64_t> srtp_decrypt_failures_ {0};
    std::atomic<std::uint64_t> audio_packets_ {0};
    std::atomic<std::uint64_t> gamepad_reports_ {0};
    std::atomic<std::uint64_t> gamepad_changes_ {0};
    std::atomic<int> transport_rtt_ms_ {-1};
    std::atomic<int> transport_rtt_p95_ms_ {-1};
};

} // namespace lunanx
