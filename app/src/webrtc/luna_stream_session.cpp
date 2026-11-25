// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=86d0cb31882f7e48e9fb
// Adapted for LunaCore on 2026-11-18.
// SPDX-License-Identifier: GPL-3.0-only

#include "luna_stream_session.hpp"

#include "core/source_origin.hpp"
#include "core/i18n.hpp"
#include "core/runtime_journal.hpp"
#include "input/luna_client_messages.hpp"
#include "luna/luna_sdp.hpp"
#include "luna/recent_games.hpp"
#include "stream/audio/AudioRtpUtils.hpp"
#include "stream/audio/AudioReorderBuffer.hpp"
#include "stream/network_loop_policy.hpp"

#include "peer.h"
#include "peer_connection.h"
#include "receive_bitrate_policy.h"
#include <opus.h>

#include <algorithm>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

LUNANX_SOURCE_ORIGIN(u86d0cb31882f7e48e9fb, "86d0cb31882f7e48e9fb")

namespace lunanx
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint16_t kInputChannel = 1;
constexpr std::uint16_t kClientChannel = 3;

// H.264 High is offered first on macOS by default; the quality profile
// chooses it on the other players.
constexpr bool kDynamicDownsampling = false;
#if defined(__APPLE__) && TARGET_OS_OSX
constexpr bool kOfferHighProfile = true;
// Arrival feedback for the server's bandwidth estimator, as Chrome sends it.
constexpr bool kTransportCc = true;
#else
constexpr bool kOfferHighProfile = false;
// Off elsewhere: on the Google TV over Wi-Fi (2026-11-22) it brought no
// higher bitrate, and thousands of retransmission requests, losses and a
// picture delay growing past half a second within two minutes, where the
// same games without it had none.
constexpr bool kTransportCc = false;
#endif

// profile-level-id the answer gives for its H.264 payload, for the journal.
std::string AnsweredH264Profile(const std::string& answer)
{
    const auto prefix = "a=fmtp:" + std::to_string(luna::AnsweredH264PayloadType(answer)) + " ";
    const auto line = answer.find(prefix);
    const auto key = line == std::string::npos ? line : answer.find("profile-level-id=", line);
    const auto end = line == std::string::npos ? line : answer.find("\r\n", line);
    if (key == std::string::npos || key > end)
        return "unspecified";
    return answer.substr(key + 17, 6);
}

void ValidateAnswer(const std::string& answer, bool fallback_offered)
{
    const auto info = luna::ParseLocalTransport(answer);
    const int video_payload = luna::AnsweredH264PayloadType(answer);
    if (info.ice_ufrag.empty() || info.ice_pwd.empty() || info.candidates.empty() ||
        !info.fingerprint.starts_with("sha-256 ") || info.fingerprint.size() != 8 + 95 ||
        (video_payload != 96 && !(fallback_offered && video_payload == 98)))
        throw std::runtime_error("Luna returned an incomplete or incompatible WebRTC answer.");
    const std::string_view fingerprint(info.fingerprint.data() + 8, 95);
    for (std::size_t i = 0; i < fingerprint.size(); ++i)
    {
        constexpr std::string_view hex = "0123456789abcdefABCDEF";
        if (i % 3 == 2 ? fingerprint[i] != ':' : hex.find(fingerprint[i]) == std::string_view::npos)
            throw std::runtime_error("Luna returned an invalid DTLS fingerprint.");
    }
    if (answer.find("a=setup:passive\r\n") != std::string::npos)
        throw std::runtime_error("The remote DTLS role is incompatible with this offer.");
}

// Media, retransmission requests and controller reports all leave from this
// thread. Let it run as soon as a datagram arrives instead of queuing behind
// UI or decoder work. Windows otherwise rounds the 1 ms waits up to 15.6 ms.
class RealtimeNetworkThread
{
  public:
    RealtimeNetworkThread()
    {
#if defined(_WIN32)
        timer_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
        // Best effort: Android permits it, desktop Linux may refuse without RLIMIT_NICE.
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), -10);
#endif
    }

    ~RealtimeNetworkThread()
    {
#if defined(_WIN32)
        if (timer_)
            timeEndPeriod(1);
#endif
    }

    RealtimeNetworkThread(const RealtimeNetworkThread&) = delete;
    RealtimeNetworkThread& operator=(const RealtimeNetworkThread&) = delete;

  private:
#if defined(_WIN32)
    bool timer_ = false;
#endif
};

} // namespace

struct LunaStreamSession::Connection
{
    explicit Connection(LunaStreamSession& session) : owner(session),
        audio_reorder(std::chrono::milliseconds(std::clamp(session.preferences_.buffer_ms / 2, 15, 30)))
    {
        if (peer_init() != 0)
            throw std::runtime_error("Could not initialize the WebRTC transport.");
        PeerConfiguration config {};
        config.video_codec = CODEC_H264;
        config.audio_codec = CODEC_OPUS;
        config.datachannel = DATA_CHANNEL_BINARY;
        config.ice_servers[0].urls = "stun:stun.l.google.com:19302";
        config.user_data = this;
        config.transport_cc = kTransportCc ? 1 : 0;
        config.onvideopacket = Video;
        config.onaudiopacket = Audio;
        peer = peer_connection_create(&config);
        if (!peer)
        {
            peer_deinit();
            throw std::runtime_error("Could not create the WebRTC connection.");
        }
        peer_connection_ondatachannel(peer, ServerMessage, nullptr, nullptr);
    }

    // The server answers the display request on the control channel; its
    // Ready message carries the bitrate ceiling it grants. Journal the first
    // messages so that the grant and the field layout can be read back.
    static void ServerMessage(char* data, std::size_t size, void* userdata, std::uint16_t sid) noexcept
    {
        auto& self = *static_cast<Connection*>(userdata);
        if (!data || self.server_messages >= 24)
            return;
        ++self.server_messages;
        try
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
            std::string hex;
            for (std::size_t i = 0; i < std::min<std::size_t>(size, 96); ++i)
            {
                char digits[3];
                std::snprintf(digits, sizeof(digits), "%02x", bytes[i]);
                hex += digits;
            }
            LogRuntimeEvent("luna", "server_message", "sid=" + std::to_string(sid) + " size=" + std::to_string(size) +
                " field=" + std::to_string(input::ServerMessageField(bytes, size)) + " hex=" + hex);
        }
        catch (...)
        {
        }
    }

    ~Connection()
    {
        PeerVideoRtpStats stats {};
        if (peer_connection_get_video_rtp_stats(peer, &stats) == 0)
        {
            owner.video_units_dropped_.store(stats.access_units_dropped, std::memory_order_relaxed);
            owner.video_rtp_packets_.store(stats.primary_packets, std::memory_order_relaxed);
            owner.video_ssrc_mismatches_.store(stats.primary_ssrc_mismatches, std::memory_order_relaxed);
            owner.srtp_decrypt_failures_.store(stats.srtp_decrypt_failures, std::memory_order_relaxed);
        }
        peer_connection_close(peer);
        peer_connection_destroy(peer);
        peer_deinit();
    }

    static void Video(const PeerVideoPacket* packet, void* userdata) noexcept
    {
        auto& self = *static_cast<Connection*>(userdata);
        if (!packet || !packet->data || packet->size == 0)
            return;
        self.owner.received_bytes_.fetch_add(packet->size, std::memory_order_relaxed);
        self.owner.video_units_.fetch_add(1, std::memory_order_relaxed);
        self.last_media = Clock::now();
        self.last_video = self.last_media;
        self.picture_lag.Add(self.last_media, packet->timestamp);
        PeerVideoRtpStats stats{};
        if (peer_connection_get_video_rtp_stats(self.peer, &stats) == 0 &&
            stats.access_units_dropped > self.last_rtp_damage)
        {
            self.last_rtp_damage = stats.access_units_dropped;
            self.awaiting_keyframe = self.needs_keyframe = true;
        }
        if (self.awaiting_keyframe && !packet->keyframe)
            return;
        if (packet->keyframe)
            self.awaiting_keyframe = self.needs_keyframe = false;

        try
        {
            if (self.owner.callbacks_.video &&
                !self.owner.callbacks_.video({packet->data, packet->size}, packet->timestamp))
                self.awaiting_keyframe = self.needs_keyframe = true;
        }
        catch (...) { self.callback_failed = true; }
    }

    static void Audio(const PeerAudioPacket* packet, void* userdata) noexcept
    {
        auto& self = *static_cast<Connection*>(userdata);
        if (!packet || !packet->data || packet->size == 0)
            return;
        self.owner.received_bytes_.fetch_add(packet->size, std::memory_order_relaxed);
        self.owner.audio_packets_.fetch_add(1, std::memory_order_relaxed);
        self.last_media = Clock::now();
        const auto payload = audio::ParseRedPrimary(packet->data, packet->size, packet->payload_type);
        try
        {
            if (self.audio_payload_type != packet->payload_type)
            {
                self.audio_payload_type = packet->payload_type;
                LogRuntimeEvent("luna", "audio_rtp_format", "payload_type=" + std::to_string(packet->payload_type));
            }
            if (self.owner.preferences_.audio && self.owner.callbacks_.audio && payload.data)
            {
                std::span<const std::uint8_t> redundant;
                // Only accept a single backup block of the immediately previous Opus packet.
                if (payload.red && packet->size > 5 && packet->data[0] == (0x80 | 111) && packet->data[4] == 111)
                {
                    const auto backup = audio::ParseFirstRedundant(packet->data, packet->size);
                    if (backup.data && opus_packet_get_nb_samples(backup.data,
                            static_cast<opus_int32>(backup.size), 48'000) == backup.timestamp_offset)
                        redundant = {backup.data, backup.size};
                }
                self.audio_reorder.Submit({payload.data, payload.size}, packet->sequence, self.last_media,
                                          self.owner.callbacks_.audio, redundant);
            }
        }
        catch (...) { self.callback_failed = true; }
    }

    bool Send(std::uint16_t channel, std::vector<std::uint8_t> message) const
    {
        return peer_connection_datachannel_send_binary_sid(
            peer, reinterpret_cast<char*>(message.data()), message.size(), channel) >= 0;
    }

    bool Open(std::uint16_t channel, const std::string& label, DecpChannelType reliability) const
    {
        std::string name = label;
        char protocol[] = "";
        return peer_connection_create_datachannel_sid(
            peer, reliability, 0, 0, name.data(), protocol, channel) >= 0;
    }

    LunaStreamSession& owner;
    audio::AudioReorderBuffer audio_reorder;
    PeerConnection* peer = nullptr;
    Clock::time_point last_media = Clock::now();
    Clock::time_point last_video {};
    bool callback_failed = false;
    int server_messages = 0;  // Network thread only.
    bool needs_keyframe = true;
    bool awaiting_keyframe = true;
    std::uint32_t last_rtp_damage = 0;
    int audio_payload_type = -1;

    // Picture lag against the sender's clock: arrival time minus capture
    // time (RTP), above the lowest value seen. It grows when pictures wait in
    // a queue on the way, the sender's own included. Network thread only.
    struct PictureLag
    {
        bool started = false;
        Clock::time_point origin {};
        std::uint32_t last_rtp = 0;
        std::int64_t capture_ticks = 0;
        double floor_ms = 0, peak_ms = 0, sum_ms = 0;
        unsigned count = 0;

        void Add(Clock::time_point arrival, std::uint32_t rtp)
        {
            if (!started)
            {
                started = true;
                origin = arrival;
                last_rtp = rtp;
            }
            capture_ticks += static_cast<std::int32_t>(rtp - last_rtp);
            last_rtp = rtp;
            const double offset = std::chrono::duration<double, std::milli>(arrival - origin).count() -
                                  static_cast<double>(capture_ticks) / 90.0;
            floor_ms = std::min(floor_ms, offset);
            const double lag = offset - floor_ms;
            peak_ms = std::max(peak_ms, lag);
            sum_ms += lag;
            ++count;
        }

        // Average and peak since the previous report.
        std::pair<int, int> Take()
        {
            const std::pair<int, int> report {count ? static_cast<int>(sum_ms / count + 0.5) : 0,
                                              static_cast<int>(peak_ms + 0.5)};
            peak_ms = sum_ms = 0;
            count = 0;
            return report;
        }
    } picture_lag;
};

LunaStreamSession::LunaStreamSession(const luna::LunaClient& client, luna::Game game, Callbacks callbacks)
    : client_(client), game_(std::move(game)), callbacks_(std::move(callbacks)),
      preferences_(ResolvedPlaybackPreferences(LoadPlaybackPreferences())), display_(PlaybackDisplay(preferences_))
{
    display_.dynamic_downsampling = kDynamicDownsampling;
}

LunaStreamSession::~LunaStreamSession() { Stop(); }

void LunaStreamSession::Start()
{
    worker_ = std::thread(&LunaStreamSession::Run, this);
}

void LunaStreamSession::Stop(bool end_remote_session)
{
    stop_.store(true);
    if (worker_.joinable())
        worker_.join();
    if (!end_remote_session)
        return;
    try
    {
        if (!session_id_.empty())
            client_.TerminateGameSession(std::exchange(session_id_, {}));
        else if (!pending_job_id_.empty())
            client_.CancelGameSessionJob(std::exchange(pending_job_id_, {}));
    }
    catch (const std::exception&)
    {
        LogRuntimeEvent("luna", "session_cleanup_failed");
    }
}

void LunaStreamSession::SetGamepad(const input::GamepadState& state)
{
    std::lock_guard lock(mutex_);
    gamepad_ = input::ApplyControllerOptions(
        state, {preferences_.swap_ab, preferences_.swap_xy, preferences_.invert_left_y});
    // The first face-button presses, pressed and sent, show in the journal
    // that the A/B and X/Y options reach the game.
    constexpr auto face = input::kA | input::kB | input::kX | input::kY;
    if ((state.buttons & face) && face_samples_ < 6)
    {
        ++face_samples_;
        char detail[48];
        std::snprintf(detail, sizeof(detail), "pressed=0x%04x sent=0x%04x", state.buttons & face, gamepad_.buttons & face);
        LogRuntimeEvent("luna", "face_buttons", detail);
    }
}

LunaStreamSession::Counters LunaStreamSession::Stats() const
{
    return {received_bytes_.load(), video_units_.load(), video_units_dropped_.load(),
            video_rtp_packets_.load(), video_ssrc_mismatches_.load(), srtp_decrypt_failures_.load(),
            audio_packets_.load(), gamepad_reports_.load(),
            gamepad_changes_.load(), transport_rtt_ms_.load(), transport_rtt_p95_ms_.load()};
}

std::string LunaStreamSession::Error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

bool LunaStreamSession::Wait(std::chrono::milliseconds delay) const
{
    const auto until = Clock::now() + delay;
    while (!stop_.load() && Clock::now() < until)
        std::this_thread::sleep_until(std::min(until, Clock::now() + 50ms));
    return !stop_.load();
}

void LunaStreamSession::Report(const std::string& status) const
{
    if (callbacks_.status)
        callbacks_.status(status);
}

void LunaStreamSession::Run()
{
    try
    {
        LogRuntimeEvent("luna", "controller_options",
                        std::string("swap_ab=") + (preferences_.swap_ab ? "1" : "0") +
                            " swap_xy=" + (preferences_.swap_xy ? "1" : "0") +
                            " invert_left_y=" + (preferences_.invert_left_y ? "1" : "0"));
        Report("Checking Luna launch eligibility");
        if (client_.IsPinChallengeRequired())
            throw std::runtime_error("Luna requires a parental PIN for this game.");
        if (game_.play_token.empty() || game_.game_id.empty())
            game_ = client_.FetchGameDetail(game_);
        if (game_.game_id.empty() || game_.play_token.empty())
            throw std::runtime_error("Luna did not provide permission to launch this game.");
        if (!stop_.load())
        {
            const RealtimeNetworkThread realtime;
            Connection connection(*this);
            Stream(connection);
        }
    }
    catch (const std::exception& ex)
    {
        LogRuntimeEvent("luna", "session_error", ex.what());
        std::lock_guard lock(mutex_);
        error_ = ex.what();
    }
    if (video_units_ > 0)
    {
        try { luna::RecordRecentGame(game_); }
        catch (const std::exception&) { LogRuntimeEvent("luna", "recent_game_write_failed"); }
    }
    finished_.store(true);
}

void LunaStreamSession::Stream(Connection& connection)
{
    const auto peer = connection.peer;
    Report("Preparing WebRTC and measuring Luna regions");
    const auto regions = client_.MeasureRegions();
    if (!regions.empty() && regions.front().latency_ms >= 0)
        Report("Nearest Luna region " + regions.front().region + ", TCP " +
               std::to_string(regions.front().latency_ms) + " ms");
    if (stop_.load())
        return;

    const char* local_sdp = peer_connection_create_offer(peer);
    const auto local = luna::ParseLocalTransport(local_sdp ? local_sdp : "");
    if (local.ice_pwd.empty() || local.fingerprint.empty() || local.candidates.empty())
        throw std::runtime_error("Could not gather a complete local WebRTC offer.");
    Report("Requesting a Luna game session");
    // The quality profile picks the H.264 profile. Ultra adds full-rate Opus;
    // its picture comes from the bitrate floor of PlaybackDisplay.
    const bool ultra = preferences_.profile == QualityProfile::Ultra;
    const auto h264 = OfferedH264Profile(preferences_.profile,
                                         kOfferHighProfile ? H264Profile::High : H264Profile::ConstrainedBaseline);
    offered_fallback_ = h264 != H264Profile::ConstrainedBaseline;
    const auto offered = h264 == H264Profile::High ? luna::OfferedProfile::High :
                         h264 == H264Profile::Main ? luna::OfferedProfile::Main : luna::OfferedProfile::ConstrainedBaseline;
    const auto offer = luna::BuildLunaOffer(local, display_.height, display_.fps, offered, kTransportCc, ultra);
    LogRuntimeEvent("luna", "quality_profile", "profile=" + std::to_string(static_cast<int>(preferences_.profile)) +
                                               " h264=" + (h264 == H264Profile::High ? "high" : h264 == H264Profile::Main ? "main" : "baseline") +
                                               " transport_cc=" + (kTransportCc ? "1" : "0"));
    // Maximum quality asks for the best encoder profile Luna offers; with the
    // standard one the encoder stays near 6 Mbit/s in fast scenes.
    const bool best = preferences_.priority == PlaybackPriority::Maximum;
    try
    {
        pending_job_id_ = client_.CreateGameSessionJob(game_, offer, input::LunaInputEncoder::RequestedChannels(),
                                                       regions, preferences_.region, best ? "BEST" : "STANDARD");
    }
    catch (const luna::LunaError& error)
    {
        if (!best || stop_.load())
            throw;
        LogRuntimeEvent("luna", "data_usage_refused", std::string("value=BEST ultra=") + (ultra ? "1" : "0") +
                                                          " error=" + error.what());
        pending_job_id_ = client_.CreateGameSessionJob(game_, offer, input::LunaInputEncoder::RequestedChannels(),
                                                       regions, preferences_.region, "STANDARD");
    }
    auto job_deadline = Clock::now() + 15min;
    luna::SessionJob job;
    auto delay = 1500ms;
    while (Wait(delay))
    {
        job = client_.GetGameSessionJob(pending_job_id_);
        if (job.status != luna::SessionJobStatus::Pending)
        {
            pending_job_id_.clear();
            session_id_ = job.session_id;
            break;
        }
        if (Clock::now() > job_deadline)
            throw std::runtime_error("Luna session provisioning timed out.");
        if (job.position_in_line > 0)
            Report("Luna queue position " + std::to_string(job.position_in_line));
        delay = std::chrono::milliseconds(std::clamp(job.retry_after_ms, 500, 5000));
    }
    if (stop_.load())
        return;
    if (job.status == luna::SessionJobStatus::Failed)
        throw std::runtime_error("Luna declined the session: " + job.error);

    const auto answer = luna::NormalizeLunaAnswer(job.answer_sdp);
    // The video line and codec names of the answer, without addresses or keys.
    {
        std::string video;
        std::istringstream lines(answer);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("m=video ") || (line.starts_with("a=rtpmap:") && line.find("/90000") != std::string::npos))
                video += (video.empty() ? "" : " | ") + line;
        }
        LogRuntimeEvent("luna", "video_answer", video.empty() ? std::string("none") : video);
    }
    ValidateAnswer(answer, offered_fallback_);
    LogRuntimeEvent("luna", "video_codec", "payload=" + std::to_string(luna::AnsweredH264PayloadType(answer)) +
        " profile_level_id=" + AnsweredH264Profile(answer) + " fallback_offered=" + (offered_fallback_ ? "yes" : "no"));
    peer_connection_set_remote_description(peer, answer.c_str(), SDP_TYPE_ANSWER);
    peer_connection_set_receive_bitrate(peer, display_.max_bitrate, display_.start_bitrate,
        preferences_.priority == PlaybackPriority::Stable ? RECEIVE_BITRATE_STABLE :
        preferences_.priority == PlaybackPriority::Maximum ? RECEIVE_BITRATE_MAXIMUM : RECEIVE_BITRATE_AUTOMATIC);
    LogRuntimeEvent("luna", "stream_transport", "region=" + job.region + " routing=" + job.routing);
    Report("Connecting to Luna over WebRTC in " + job.region);

    input::LunaInputEncoder encoder;
    input::GamepadState previous;
    bool input_open = false;
    bool client_open = false;
    bool input_ready = false;
    std::size_t startup_sent = 0;
    int display_retries = 0;
    const auto display_request = input::EncodeDisplayRequest(display_);
    const std::vector<std::uint8_t> startup[] = {
        input::EncodeSessionHello(), display_request, input::EncodeClientInfo()};
    auto last_attempt = Clock::time_point {};
    auto last_display = Clock::time_point {};
    auto last_keyframe = Clock::time_point {};
    auto last_picture = Clock::time_point {};
    auto last_report = Clock::time_point {};
    unsigned input_repeats = 0;
    auto repeat_after = Clock::time_point {};
    auto send_input = [&](Clock::time_point now) {
        if (!input_ready) return;
        input::GamepadState current;
        {
            std::lock_guard lock(mutex_);
            current = gamepad_;
        }
        const bool changed = current != previous;
        const bool repeat = input_repeats && now >= repeat_after;
        if ((!changed && !repeat && now - last_report < 500ms) ||
            !connection.Send(kInputChannel, encoder.EncodeGamepad(current)))
            return;
        previous = current;
        last_report = now;
        gamepad_reports_.fetch_add(1, std::memory_order_relaxed);
        if (changed)
        {
            gamepad_changes_.fetch_add(1, std::memory_order_relaxed);
            input_repeats = 2;
            repeat_after = now + 12ms;
        }
        else if (repeat)
        {
            --input_repeats;
            repeat_after = now + 20ms;
        }
    };
    auto last_ping = Clock::time_point {};
    auto last_rtt = Clock::time_point {};
    auto last_quality = Clock::now();
    std::uint64_t quality_bytes = 0, quality_units = 0;
    auto completed_at = Clock::time_point {};
    auto last_state = PEER_CONNECTION_NEW;
    connection.last_media = Clock::now();
    while (!stop_.load())
    {
        const auto batch_start = Clock::now();
        send_input(batch_start);
        int processed = 0;
        for (; processed < 24; ++processed)
        {
            if ((processed == 0 ? peer_connection_loop(peer) : peer_connection_loop_nonblocking(peer)) <= 0)
                break;
            if ((processed + 1) % 8 == 0 && Clock::now() - batch_start >= 2ms)
            {
                ++processed;
                break;
            }
        }
        peer_connection_flush_pending_media(peer);
        const auto state = peer_connection_get_state(peer);
        const auto now = Clock::now();
        if (!connection.callback_failed && callbacks_.audio)
            connection.audio_reorder.Tick(now, callbacks_.audio);
        if (last_state != state)
        {
            last_state = state;
            if (state == PEER_CONNECTION_COMPLETED)
                completed_at = now;
            Report("WebRTC " + std::string(peer_connection_state_to_string(state)));
        }
        if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_CLOSED ||
            state == PEER_CONNECTION_DISCONNECTED || now - connection.last_media > 35s)
            throw std::runtime_error("Luna media connection stopped (" +
                                     std::string(peer_connection_state_to_string(state)) + ").");
        if (connection.callback_failed)
            throw std::runtime_error("A media callback failed.");

        if (state == PEER_CONNECTION_COMPLETED)
        {
            const auto video_idle = now - std::max(completed_at, connection.last_video);
            if (video_idle > 20s)
            {
                LogRuntimeEvent("luna", "video_timeout", "startup_sent=" + std::to_string(startup_sent) +
                    " display_retries=" + std::to_string(display_retries) +
                    " sctp_ready=" + (peer_connection_datachannel_ready(peer) ? "yes" : "no"));
                throw std::runtime_error(Tr("Luna ne fournit pas d’image pour ce jeu. Ferme la partie et réessaie."));
            }
            if (now - last_rtt >= 1s)
            {
                last_rtt = now;
                PeerNetworkRttStats stats {};
                if (peer_connection_get_rtt_stats(peer, &stats) == 0)
                {
                    transport_rtt_ms_.store(stats.current_ms);
                    transport_rtt_p95_ms_.store(stats.p95_ms);
                }
            }
            if (now - last_quality >= 10s)
            {
                // What actually arrived, beside the limit the sender was given.
                const double seconds = std::chrono::duration<double>(now - last_quality).count();
                const auto bytes = received_bytes_.load(), units = video_units_.load();
                const auto received_kbps = static_cast<std::uint64_t>((bytes - quality_bytes) * 8 / seconds / 1000);
                const auto received_fps = static_cast<std::uint64_t>((units - quality_units) / seconds + 0.5);
                quality_bytes = bytes;
                quality_units = units;
                last_quality = now;
                PeerVideoRtpStats stats{};
                const auto [lag_ms, lag_peak_ms] = connection.picture_lag.Take();
                if (peer_connection_get_video_rtp_stats(peer, &stats) == 0)
                    LogRuntimeEvent("luna", "stream_quality", "received_kbps=" + std::to_string(received_kbps) +
                        " received_fps=" + std::to_string(received_fps) + " picture_lag_ms=" +
                        std::to_string(lag_ms) + " picture_lag_peak_ms=" + std::to_string(lag_peak_ms) +
                        " received_kbps_limit=" +
                        std::to_string(stats.target_bitrate / 1000) + " video_loss_q8=" +
                        std::to_string(stats.video_loss_q8) + " audio_loss_q8=" +
                        std::to_string(stats.audio_loss_q8) + " damaged_frames=" +
                        std::to_string(stats.access_units_dropped) + " recovered_rtx=" +
                        std::to_string(stats.rtx_packets_recovered) + " rtx_received=" +
                        std::to_string(stats.rtx_packets_received) + " repair_q8=" +
                        std::to_string(stats.video_repair_q8) + " nack_packets=" +
                        std::to_string(stats.nack_packets_requested) + " srtp_failures=" +
                        std::to_string(stats.srtp_decrypt_failures) + " red_audio_repairs=" +
                        std::to_string(connection.audio_reorder.RedRepairs()) + " rtt_ms=" +
                        std::to_string(transport_rtt_ms_.load()) + " twcc_extension=" +
                        std::to_string(stats.transport_extension_id) + " twcc_packets=" +
                        std::to_string(stats.transport_packets) + " twcc_feedback=" +
                        std::to_string(stats.transport_feedback_sent));
            }
            if (peer_connection_datachannel_ready(peer) && now - last_attempt >= 750ms &&
                (!input_ready || startup_sent < std::size(startup)))
            {
                last_attempt = now;
                input_open = input_open || connection.Open(kInputChannel,
                    input::LunaInputEncoder::InputChannelLabel(), DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED);
                client_open = client_open || connection.Open(kClientChannel,
                    input::LunaInputEncoder::ClientMessageChannelLabel(), DATA_CHANNEL_RELIABLE);
                if (input_open && !input_ready)
                    input_ready = connection.Send(kInputChannel, input::LunaInputEncoder::EncodeInputReady());
                while (client_open && startup_sent < std::size(startup) &&
                       connection.Send(kClientChannel, startup[startup_sent]))
                {
                    ++startup_sent;
                    if (startup_sent == std::size(startup))
                    {
                        last_display = now;
                        LogRuntimeEvent("luna", "stream_display_sent", "height=" + std::to_string(display_.height) +
                            " fps=" + std::to_string(display_.fps) +
                            " bitrate_mbps=" + std::to_string(display_.max_bitrate / 1'000'000) +
                            " start_kbps=" + std::to_string(display_.start_bitrate / 1000) +
                            " min_kbps=" + std::to_string(display_.min_bitrate / 1000) +
                            " dynamic_downsampling=" + (display_.dynamic_downsampling ? "yes" : "no"));
                    }
                }
            }
            // Luna may send audio before processing the display request on the control channel.
            if (startup_sent == std::size(startup) && connection.last_video == Clock::time_point {} &&
                display_retries < 3 && now - last_display >= 2s)
            {
                last_display = now;
                ++display_retries;
                const bool sent = connection.Send(kClientChannel, display_request);
                LogRuntimeEvent("luna", "stream_display_retry", "sent=" + std::to_string(sent));
            }
            if (startup_sent == std::size(startup) && now - last_ping >= 1s)
            {
                last_ping = now;
                connection.Send(kClientChannel, input::EncodePing(input::WallClockMilliseconds()));
            }
            if (!connection.awaiting_keyframe) last_picture = connection.last_video;
            // While the picture is frozen, ask again once a keyframe could have
            // arrived: an IDR lost to the same burst used to cost another 450 ms.
            const auto rtt = std::chrono::milliseconds(std::max(0, transport_rtt_ms_.load()));
            const auto keyframe_interval = last_picture >= last_keyframe ? 150ms :
                std::clamp<std::chrono::milliseconds>(250ms + rtt * 2, 250ms, 450ms);
            if ((connection.needs_keyframe || connection.awaiting_keyframe || video_idle >= 2s) &&
                now - last_keyframe >= keyframe_interval)
            {
                last_keyframe = now;
                connection.needs_keyframe = peer_connection_request_video_keyframe(peer) < 0;
            }
            send_input(Clock::now());
        }
        // Once media flows, the 1 ms socket wait already paces the loop. An extra
        // sleep only delayed controller reports and incoming frames. Keep it if
        // the wait returned at once, so a failing socket cannot spin the CPU.
        const bool socket_waited = state == PEER_CONNECTION_COMPLETED &&
                                   Clock::now() - batch_start >= 500us;
        if (const int backoff = network::LoopBackoffMilliseconds(socket_waited, processed); backoff > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
}

} // namespace lunanx
