// SPDX-License-Identifier: GPL-3.0-only

#include "player.hpp"
#include "core/runtime_journal.hpp"
#include "core/playback_preferences.hpp"
#include "stream/performance_overlay.hpp"
#include "stream/audio/AudioLatencyPolicy.hpp"
#include "stream/audio/OpusRecovery.hpp"
#include "stream/audio/PcmBuffer.hpp"
#include "stream/DecodeQueuePolicy.hpp"
#include "webrtc/luna_stream_session.hpp"

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>
#include <opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lunaone::android
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::mutex input_mutex;
std::condition_variable input_changed;
lunanx::input::GamepadState gamepad;

class Video
{
  public:
    Video(ANativeWindow* surface, const lunanx::PlaybackPreferences& settings, const std::string& decoder)
        : queue_limit_(lunanx::video::MaximumBurstAccessUnits(settings.fps))
    {
        codec_.reset(decoder.empty() ? AMediaCodec_createDecoderByType("video/avc")
                                     : AMediaCodec_createCodecByName(decoder.c_str()));
        if (!codec_)
            throw std::runtime_error("Aucun décodeur H.264 Android disponible.");
        const auto profile = lunanx::PlaybackDisplay(settings);
        using Format = std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)>;
        const auto make_format = [&profile](bool low_latency) {
            Format format(AMediaFormat_new(), &AMediaFormat_delete);
            if (!format)
                throw std::runtime_error("Impossible de préparer le format vidéo Android.");
            AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, "video/avc");
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, profile.width);
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, profile.height);
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_FRAME_RATE, profile.fps);
            if (!low_latency)
                return format;
            // Without these, many TV SoCs hold several pictures before the first
            // output, which adds latency and lets the decode queue overflow.
            for (const char* key : {"low-latency",                                  // Android 11 KEY_LOW_LATENCY
                                    "vendor.qti-ext-dec-low-latency.enable",          // Qualcomm
                                    "vendor.qti-ext-dec-picture-order.enable",
                                    "vendor.low-latency.enable",                      // Amlogic
                                    "vendor.rtc-ext-dec-low-latency.enable",          // Exynos
                                    "vdec-lowlatency",                                // MediaTek
                                    "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req"})
                AMediaFormat_setInt32(format.get(), key, 1);
            AMediaFormat_setInt32(format.get(),
                "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy", -1);
            AMediaFormat_setInt32(format.get(), "priority", 0);  // Real time.
            return format;
        };
        // Codecs ignore unknown keys. One that rejects them is configured again without.
        bool configured = AMediaCodec_configure(codec_.get(), make_format(true).get(), surface, nullptr, 0) == AMEDIA_OK;
        low_latency_ = configured;
        if (!configured)
        {
            // A failed configure leaves the codec unusable. Start from a fresh instance.
            codec_.reset(decoder.empty() ? AMediaCodec_createDecoderByType("video/avc")
                                         : AMediaCodec_createCodecByName(decoder.c_str()));
            configured = codec_ &&
                AMediaCodec_configure(codec_.get(), make_format(false).get(), surface, nullptr, 0) == AMEDIA_OK;
        }
        if (!configured || AMediaCodec_start(codec_.get()) != AMEDIA_OK)
            throw std::runtime_error("Impossible de démarrer MediaCodec pour le flux H.264.");
        lunanx::LogRuntimeEvent("android", "video_decoder", "name=" + (decoder.empty() ? "default" : decoder) +
            " low_latency=" + (low_latency_ ? "yes" : "no"));
        worker_ = std::thread([this] { Decode(); });
    }

    ~Video()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable())
            worker_.join();
        AMediaCodec_stop(codec_.get());
    }

    bool Submit(std::span<const std::uint8_t> bytes, std::uint32_t timestamp)
    {
        const auto now = Clock::now();
        std::lock_guard lock(mutex_);
        if (queue_.size() >= queue_limit_ || queued_bytes_ >= lunanx::video::MaximumBurstQueueBytes ||
            bytes.size() > lunanx::video::MaximumBurstQueueBytes - queued_bytes_ ||
            (!queue_.empty() && now - queue_.front().arrival >
                std::chrono::milliseconds(lunanx::video::MaximumBurstQueueDelayMs)))
        {
            queue_.clear();
            queued_bytes_ = 0;
            needs_idr_ = true;
            discontinuity_ = true;
            ++generation_;
            ++queue_resets_;
        }
        if (bytes.size() > lunanx::video::MaximumBurstQueueBytes ||
            (needs_idr_ && !lunanx::video::IsH264Keyframe(bytes)))
            return false;
        queue_.push_back({{bytes.begin(), bytes.end()}, now, timestamp, discontinuity_, generation_});
        queued_bytes_ += bytes.size();
        peak_queue_.store(std::max(peak_queue_.load(std::memory_order_relaxed),
            static_cast<std::uint32_t>(queue_.size())), std::memory_order_relaxed);
        needs_idr_ = false;
        discontinuity_ = false;
        wake_.notify_one();
        return true;
    }

    std::uint64_t Frames() const { return frames_; }
    int Error() const { return error_; }
    std::uint32_t QueueResets() const { return queue_resets_; }
    std::uint32_t LateFrames() const { return late_frames_; }
    std::uint32_t PeakQueue() const { return peak_queue_; }
    int Height() const { return decoded_height_; }

  private:
    struct Packet
    {
        std::vector<std::uint8_t> bytes;
        Clock::time_point arrival;
        std::uint32_t timestamp;
        bool discontinuity;
        std::uint32_t generation;
    };

    struct Picture
    {
        std::size_t index;
        std::chrono::microseconds pts;
    };

    void UpdateFormat()
    {
        const auto format = std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)>(
            AMediaCodec_getOutputFormat(codec_.get()), &AMediaFormat_delete);
        int32_t width = 0, height = 0, left = 0, top = 0, right = 0, bottom = 0;
        if (format && AMediaFormat_getInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, &width) &&
            AMediaFormat_getInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, &height))
        {
            if (AMediaFormat_getInt32(format.get(), "crop-left", &left) &&
                AMediaFormat_getInt32(format.get(), "crop-right", &right) &&
                AMediaFormat_getInt32(format.get(), "crop-top", &top) &&
                AMediaFormat_getInt32(format.get(), "crop-bottom", &bottom) && right >= left && bottom >= top)
            {
                width = right - left + 1;
                height = bottom - top + 1;
            }
            decoded_height_.store(height, std::memory_order_relaxed);
            lunanx::LogRuntimeEvent("android", "decoded_format",
                "width=" + std::to_string(width) + " height=" + std::to_string(height));
        }
    }

    std::optional<Picture> ReadPicture()
    {
        for (int i = 0; i < 8; ++i)
        {
            AMediaCodecBufferInfo info{};
            const auto index = AMediaCodec_dequeueOutputBuffer(codec_.get(), &info, 0);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
                break;
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
            {
                UpdateFormat();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (index < 0)
            {
                error_ = static_cast<int>(index);
                break;
            }
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
                return Picture{static_cast<std::size_t>(index), std::chrono::microseconds(info.presentationTimeUs)};
            if (const auto status = AMediaCodec_releaseOutputBuffer(codec_.get(), index, false); status != AMEDIA_OK)
            {
                error_ = status;
                break;
            }
        }
        return {};
    }

    // Game streaming: show the newest decoded picture at once. A picture that a
    // newer one overtook in the same burst is released without rendering, so
    // the display never queues behind the stream. The former schedule held
    // every picture about 30 ms before display.
    void Present()
    {
        std::optional<Picture> newest;
        for (int i = 0; i < 8 && !Error(); ++i)
        {
            auto picture = ReadPicture();
            if (!picture)
                break;
            if (newest)
            {
                ++late_frames_;
                if (const auto status = AMediaCodec_releaseOutputBuffer(codec_.get(), newest->index, false);
                    status != AMEDIA_OK)
                {
                    error_ = status;
                    return;
                }
            }
            newest = picture;
        }
        if (!newest || Error())
            return;
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
        if (const auto status = AMediaCodec_releaseOutputBufferAtTime(codec_.get(), newest->index, now);
            status != AMEDIA_OK)
        {
            error_ = status;
            return;
        }
        ++frames_;
    }

    void Decode() noexcept
    {
        std::uint32_t start_time = 0;
        bool first = true;
        for (;;)
        {
            Packet packet{};
            {
                std::unique_lock lock(mutex_);
                // Also the polling period for decoded pictures.
                wake_.wait_for(lock, 1ms, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                if (!queue_.empty())
                {
                    packet = std::move(queue_.front());
                    queue_.pop_front();
                    queued_bytes_ -= packet.bytes.size();
                }
            }
            if (packet.discontinuity)
            {
                const auto status = AMediaCodec_flush(codec_.get());
                if (status != AMEDIA_OK)
                {
                    error_ = status;
                    return;
                }
                packet.discontinuity = false;
                first = true;
            }
            // Free available output buffers before waiting for another input slot.
            Present();
            if (Error()) return;
            if (packet.bytes.empty()) continue;

            const auto index = AMediaCodec_dequeueInputBuffer(codec_.get(), 1500);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                std::lock_guard lock(mutex_);
                if (!needs_idr_ && packet.generation == generation_)
                {
                    queued_bytes_ += packet.bytes.size();
                    queue_.push_front(std::move(packet));
                }
                continue;
            }
            if (index < 0)
            {
                error_ = static_cast<int>(index);
                return;
            }
            std::size_t capacity = 0;
            auto* buffer = AMediaCodec_getInputBuffer(codec_.get(), index, &capacity);
            if (!buffer || capacity < packet.bytes.size())
            {
                error_ = AMEDIA_ERROR_MALFORMED;
                return;
            }
            if (first)
            {
                start_time = packet.timestamp;
                first = false;
            }
            std::memcpy(buffer, packet.bytes.data(), packet.bytes.size());
            const auto pts = static_cast<std::uint32_t>(packet.timestamp - start_time) * 1'000'000ULL / 90'000;
            const auto status = AMediaCodec_queueInputBuffer(codec_.get(), index, 0, packet.bytes.size(), pts, 0);
            if (status != AMEDIA_OK)
            {
                error_ = status;
                return;
            }
        }
    }

    std::unique_ptr<AMediaCodec, decltype(&AMediaCodec_delete)> codec_{nullptr, &AMediaCodec_delete};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Packet> queue_;
    const std::size_t queue_limit_;
    std::size_t queued_bytes_ = 0;
    std::atomic<std::uint64_t> frames_ = 0;
    std::atomic<int> error_ = 0, decoded_height_ = 0;
    std::atomic<std::uint32_t> queue_resets_{0}, late_frames_{0}, peak_queue_{0};
    std::uint32_t generation_ = 0;
    bool needs_idr_ = true, discontinuity_ = false, stopping_ = false;
    bool low_latency_ = false;
    std::thread worker_;
};

class Audio
{
  public:
    explicit Audio(const lunanx::PlaybackPreferences& settings)
        : volume_(settings.volume), buffer_(settings.buffer_ms)
    {
        if (!settings.audio) return;
        decoder_.reset(opus_decoder_create(48'000, 2, nullptr));
        AAudioStreamBuilder* builder = nullptr;
        if (!decoder_ || AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
            return;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSampleRate(builder, 48'000);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setDataCallback(builder, Render, this);
        if (AAudioStreamBuilder_openStream(builder, &stream_) != AAUDIO_OK)
            stream_ = nullptr;
        AAudioStreamBuilder_delete(builder);
    }

    ~Audio()
    {
        if (stream_)
        {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
        }
    }

    void Submit(std::span<const std::uint8_t> bytes, std::uint16_t sequence)
    {
        if (!stream_)
            return;
        const int frames = recovery_.Decode(decoder_.get(), bytes, sequence, pcm_);
        if (frames <= 0)
            return;
        const auto samples = static_cast<std::size_t>(frames) * 2;
        lunanx::audio::ApplyVolume({pcm_.data(), samples}, volume_);
        const auto queued = buffer_.Submit({pcm_.data(), samples});
        if (!started_ && queued >= buffer_.Target())
            started_ = AAudioStream_requestStart(stream_) == AAUDIO_OK;
    }

    std::uint32_t Underruns() const { return buffer_.Underruns(); }
    std::uint32_t Dropped() const { return buffer_.Dropped(); }
    std::uint32_t Trimmed() const { return buffer_.TrimmedFrames(); }
    int TargetMs() const { return buffer_.TargetMs(); }
    // Device underruns, which the playback buffer cannot see.
    std::int32_t DeviceUnderruns() const { return stream_ ? AAudioStream_getXRunCount(stream_) : 0; }

  private:
    static aaudio_data_callback_result_t Render(AAudioStream*, void* userdata, void* data, std::int32_t frames)
    {
        auto& audio = *static_cast<Audio*>(userdata);
        audio.buffer_.Render({static_cast<std::int16_t*>(data), static_cast<std::size_t>(frames) * 2});
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder_{nullptr, &opus_decoder_destroy};
    AAudioStream* stream_{};
    const int volume_;
    lunanx::audio::PcmBuffer buffer_;
    std::array<std::int16_t, 5760 * 2> pcm_{};
    lunanx::audio::OpusRecovery recovery_;
    bool started_ = false;
};
}

void UpdateGamepad(lunanx::input::GamepadState state)
{
    {
        std::lock_guard lock(input_mutex);
        if (gamepad == state) return;
        gamepad = state;
    }
    input_changed.notify_one();
}

void Play(const lunanx::luna::LunaClient& client, lunanx::luna::Game game, ANativeWindow* surface,
          const lunanx::PlaybackPreferences& settings, const std::string& decoder,
          const std::function<bool()>& cancelled,
          const std::function<void(const std::string&)>& status,
          const std::function<void(const std::string&)>& metrics)
{
    if (cancelled())
        return;
    Video video(surface, settings, decoder);
    Audio audio(settings);
    std::mutex status_mutex;
    std::string latest_status;
    lunanx::LunaStreamSession session(client, std::move(game), {
        [&](auto bytes, auto pts) { return video.Submit(bytes, pts); },
        [&](auto bytes, auto sequence) { audio.Submit(bytes, sequence); },
        [&](const std::string& value) {
            std::lock_guard lock(status_mutex);
            latest_status = value;
        }});
    session.Start();
    std::string failure;
    try
    {
        Clock::time_point first_video{};
        Clock::time_point next_metrics{};
        lunanx::PerformanceOverlay overlay(settings.diagnostics);
        bool overlay_shown = false;
        bool rendering = false;
        while (!session.Finished() && !cancelled())
        {
            if (const int error = video.Error())
                throw std::runtime_error("MediaCodec a interrompu le décodage (" + std::to_string(error) + ").");
            if (!rendering)
            {
                std::string message;
                {
                    std::lock_guard lock(status_mutex);
                    message.swap(latest_status);
                }
                if (video.Frames() > 0)
                {
                    status({});
                    rendering = true;
                }
                else if (!message.empty())
                {
                    status(message);
                }
                if (first_video == Clock::time_point{} && session.Stats().video_units > 0)
                    first_video = Clock::now();
                if (first_video != Clock::time_point{} && Clock::now() - first_video > 12s && !rendering)
                    throw std::runtime_error("Le flux vidéo est reçu, mais MediaCodec n’a affiché aucune image.");
            }
            lunanx::input::GamepadState sent_input;
            {
                std::lock_guard lock(input_mutex);
                sent_input = gamepad;
            }
            if (lunanx::input::ExitStreamChord(sent_input)) break;
            session.SetGamepad(overlay.Update(sent_input));
            const auto now = Clock::now();
            if (overlay_shown != overlay.Visible() || now >= next_metrics)
            {
                overlay.Sample({session.Stats().received_bytes, video.Frames()});
                if (overlay.Visible())
                    metrics(overlay.Format({}, -1, video.Height()));
                else if (overlay_shown)
                    metrics("");
                overlay_shown = overlay.Visible();
                next_metrics = now + 1s;
            }
            std::unique_lock lock(input_mutex);
            input_changed.wait_for(lock, 6ms, [&] { return gamepad != sent_input; });
        }
        failure = session.Error();
    }
    catch (...)
    {
        session.Stop(true);
        throw;
    }
    session.Stop(true);
    const auto counters = session.Stats();
    lunanx::LogRuntimeEvent("android", "stream_metrics", "frames=" + std::to_string(video.Frames()) +
        " queue_resets=" + std::to_string(video.QueueResets()) +
        " queue_peak=" + std::to_string(video.PeakQueue()) +
        " late_frames=" + std::to_string(video.LateFrames()) +
        " audio_underruns=" + std::to_string(audio.Underruns()) +
        " audio_dropped=" + std::to_string(audio.Dropped()) +
        " audio_trimmed_frames=" + std::to_string(audio.Trimmed()) +
        " audio_target_ms=" + std::to_string(audio.TargetMs()) +
        " audio_device_underruns=" + std::to_string(audio.DeviceUnderruns()) +
        " video_units=" + std::to_string(counters.video_units) +
        " audio_packets=" + std::to_string(counters.audio_packets) +
        " gamepad_changes=" + std::to_string(counters.gamepad_changes));
    if (!cancelled() && !failure.empty())
        throw std::runtime_error(failure);
}
} // namespace lunaone::android
