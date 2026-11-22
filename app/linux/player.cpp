// SPDX-License-Identifier: GPL-3.0-only
#include "player.hpp"
#include "artwork.hpp"
#include "stream/launch_status.hpp"
#include "core/i18n.hpp"
#include "stream/performance_overlay.hpp"
#include "core/playback_preferences.hpp"
#include "stream/audio/AudioLatencyPolicy.hpp"
#include "stream/audio/OpusRecovery.hpp"
#include "stream/audio/PcmBuffer.hpp"
#include "stream/DecodeQueuePolicy.hpp"
#include "webrtc/luna_stream_session.hpp"

#include <SDL.h>
#include <opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <filesystem>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace lunaone::linux_desktop
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using namespace lunanx::input;

struct Frame
{
    int width{}, height{};
    std::vector<std::uint8_t> pixels;
};

class Decoder
{
  public:
    explicit Decoder(int fps) : queue_limit_(lunanx::video::MaximumBurstAccessUnits(fps))
    {
        context_ = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
        if (context_)
        {
            context_->thread_count = 2;
            context_->thread_type = FF_THREAD_SLICE;
            context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        }
        if (!context_ || !picture_ || !packet_ || avcodec_open2(context_, nullptr, nullptr) < 0)
        {
            Release();
            throw std::runtime_error("H.264 decoder unavailable.");
        }
        try { worker_ = std::thread([this] { Decode(); }); }
        catch (...) { Release(); throw; }
    }

    ~Decoder()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        worker_.join();
        Release();
    }

    bool Submit(std::span<const std::uint8_t> bytes)
    {
        const auto now = Clock::now();
        std::lock_guard lock(mutex_);
        if (queue_.size() >= queue_limit_ ||
            bytes.size() > lunanx::video::MaximumBurstQueueBytes - queued_bytes_ ||
            (!queue_.empty() && now - queue_.front().arrival >
                std::chrono::milliseconds(lunanx::video::MaximumBurstQueueDelayMs)))
            ResetQueue();
        if (bytes.size() > lunanx::video::MaximumBurstQueueBytes ||
            (needs_idr_ && !lunanx::video::IsH264Keyframe(bytes)))
            return false;
        queue_.push_back({{bytes.begin(), bytes.end()}, now, generation_, needs_idr_});
        queued_bytes_ += bytes.size();
        needs_idr_ = false;
        wake_.notify_one();
        return true;
    }

    std::shared_ptr<Frame> TakeFrame()
    {
        std::lock_guard lock(mutex_);
        return std::exchange(latest_, {});
    }

  private:
    struct Unit
    {
        std::vector<std::uint8_t> bytes;
        Clock::time_point arrival;
        std::uint64_t generation;
        bool reset;
    };

    void ResetQueue()
    {
        queue_.clear();
        queued_bytes_ = 0;
        ++generation_;
        needs_idr_ = true;
    }

    void Decode()
    {
        for (;;)
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            auto unit = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= unit.bytes.size();
            lock.unlock();
            if (unit.reset) avcodec_flush_buffers(context_);
            bool decoded = false;
            try { decoded = DecodeUnit(unit); }
            catch (const std::exception&) {}
            if (!decoded)
            {
                lock.lock();
                if (unit.generation == generation_) ResetQueue();
            }
        }
    }

    bool DecodeUnit(const Unit& unit)
    {
        const auto& bytes = unit.bytes;
        av_packet_unref(packet_);
        if (av_new_packet(packet_, static_cast<int>(bytes.size())) < 0)
            return false;
        std::memcpy(packet_->data, bytes.data(), bytes.size());
        if (avcodec_send_packet(context_, packet_) < 0)
            return false;
        while (avcodec_receive_frame(context_, picture_) >= 0)
        {
            const int width = picture_->width, height = picture_->height;
            if (width < 1 || height < 1 || width > 4096 || height > 2304)
                return false;
            scale_ = sws_getCachedContext(scale_, width, height, static_cast<AVPixelFormat>(picture_->format),
                                          width, height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!scale_)
                return false;
            auto output = std::make_shared<Frame>();
            output->width = width;
            output->height = height;
            output->pixels.resize(static_cast<std::size_t>(width * height) * 4);
            std::uint8_t* planes[] {output->pixels.data()};
            const int strides[] {width * 4};
            sws_scale(scale_, picture_->data, picture_->linesize, 0, height, planes, strides);
            std::lock_guard lock(mutex_);
            if (unit.generation == generation_) latest_ = std::move(output);
        }
        return true;
    }

    void Release()
    {
        if (scale_) sws_freeContext(scale_);
        av_packet_free(&packet_);
        av_frame_free(&picture_);
        avcodec_free_context(&context_);
    }

    AVCodecContext* context_{};
    AVFrame* picture_ = av_frame_alloc();
    AVPacket* packet_ = av_packet_alloc();
    SwsContext* scale_{};
    std::mutex mutex_;
    std::shared_ptr<Frame> latest_;
    std::condition_variable wake_;
    std::deque<Unit> queue_;
    const std::size_t queue_limit_;
    std::size_t queued_bytes_ = 0;
    std::uint64_t generation_ = 0;
    bool needs_idr_ = true, stopping_ = false;
    std::thread worker_;
};

class AudioAndInput
{
  public:
    explicit AudioAndInput(const lunanx::PlaybackPreferences& settings)
        : volume_(settings.volume), buffer_(settings.buffer_ms)
    {
        if (SDL_Init((settings.audio ? SDL_INIT_AUDIO : 0) | SDL_INIT_GAMECONTROLLER) < 0)
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_AudioSpec requested{};
        requested.freq = 48'000;
        requested.format = AUDIO_S16SYS;
        requested.channels = 2;
        requested.samples = 512;
        requested.callback = RenderAudio;
        requested.userdata = this;
        if (decoder_ && settings.audio)
            audio_ = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
    }

    ~AudioAndInput()
    {
        if (audio_) SDL_CloseAudioDevice(audio_);
        if (controller_) SDL_GameControllerClose(controller_);
        if (decoder_) opus_decoder_destroy(decoder_);
        SDL_Quit();
    }

    void Submit(std::span<const std::uint8_t> packet, std::uint16_t sequence)
    {
        if (!audio_)
            return;
        const int samples = recovery_.Decode(decoder_, packet, sequence, pcm_);
        if (samples <= 0)
            return;
        lunanx::audio::ApplyVolume({pcm_.data(), static_cast<std::size_t>(samples) * 2}, volume_);
        const auto queued = buffer_.Submit({pcm_.data(), static_cast<std::size_t>(samples) * 2});
        if (!unpaused_ && queued >= buffer_.Target())
        {
            SDL_PauseAudioDevice(audio_, 0);
            unpaused_ = true;
        }
    }

    GamepadState Read()
    {
        SDL_PumpEvents();
        if (controller_ && !SDL_GameControllerGetAttached(controller_))
        {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
        if (!controller_ && Clock::now() >= next_scan_)
        {
            next_scan_ = Clock::now() + 500ms;
            for (int i = 0; i < SDL_NumJoysticks() && !controller_; ++i)
                if (SDL_IsGameController(i))
                    controller_ = SDL_GameControllerOpen(i);
        }
        GamepadState state;
        if (!controller_) return state;
        for (auto [button, mask] : {std::pair{SDL_CONTROLLER_BUTTON_A, kA}, std::pair{SDL_CONTROLLER_BUTTON_B, kB},
            std::pair{SDL_CONTROLLER_BUTTON_X, kX}, std::pair{SDL_CONTROLLER_BUTTON_Y, kY},
            std::pair{SDL_CONTROLLER_BUTTON_DPAD_UP, kDpadUp}, std::pair{SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDpadDown},
            std::pair{SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDpadLeft}, std::pair{SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDpadRight},
            std::pair{SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLeftShoulder}, std::pair{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRightShoulder},
            std::pair{SDL_CONTROLLER_BUTTON_LEFTSTICK, kLeftThumb}, std::pair{SDL_CONTROLLER_BUTTON_RIGHTSTICK, kRightThumb},
            std::pair{SDL_CONTROLLER_BUTTON_START, kStart}, std::pair{SDL_CONTROLLER_BUTTON_BACK, kBack}})
            if (SDL_GameControllerGetButton(controller_, button))
                state.buttons |= mask;
        const auto axis = [this](SDL_GameControllerAxis id) { return SDL_GameControllerGetAxis(controller_, id); };
        state.left_x = axis(SDL_CONTROLLER_AXIS_LEFTX);
        state.left_y = static_cast<std::int16_t>(-std::max<int>(axis(SDL_CONTROLLER_AXIS_LEFTY), -32767));
        state.right_x = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        state.right_y = static_cast<std::int16_t>(-std::max<int>(axis(SDL_CONTROLLER_AXIS_RIGHTY), -32767));
        state.left_trigger = static_cast<std::uint8_t>(std::max<int>(0, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)) * 255 / 32767);
        state.right_trigger = static_cast<std::uint8_t>(std::max<int>(0, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) * 255 / 32767);
        return state;
    }

  private:
    static void RenderAudio(void* userdata, Uint8* bytes, int length)
    {
        auto& self = *static_cast<AudioAndInput*>(userdata);
        self.buffer_.Render({reinterpret_cast<std::int16_t*>(bytes), static_cast<std::size_t>(length) / 2});
    }

    SDL_GameController* controller_{};
    const int volume_;
    lunanx::audio::PcmBuffer buffer_;
    SDL_AudioDeviceID audio_{};
    OpusDecoder* decoder_ = opus_decoder_create(48000, 2, nullptr);
    std::array<std::int16_t, 5760 * 2> pcm_{};
    Clock::time_point next_scan_{};
    lunanx::audio::OpusRecovery recovery_;
    bool unpaused_ = false;
};

std::string Network()
{
    std::ifstream routes("/proc/net/route");
    std::string name, destination, line;
    std::getline(routes, line);
    while (routes >> name >> destination)
    {
        std::getline(routes, line);
        if (destination != "00000000") continue;
        if (name.starts_with("wwan") || name.starts_with("rmnet")) return "Mobile";
        std::error_code error;
        const std::filesystem::path interface = "/sys/class/net/" + name;
        if (std::filesystem::exists(interface / "wireless", error) ||
            std::filesystem::exists(interface / "phy80211", error))
            return "Wi-Fi";
        std::ifstream type(interface / "type");
        int link_type = 0;
        if ((type >> link_type) && link_type == 1) return "LAN";
    }
    return {};
}

int Battery()
{
    std::error_code error;
    auto entries = std::filesystem::directory_iterator("/sys/class/power_supply", error);
    for (const auto& entry : entries)
    {
        std::ifstream type(entry.path() / "type");
        std::string value;
        if (!(type >> value) || value != "Battery") continue;
        std::ifstream capacity(entry.path() / "capacity");
        int percentage = -1;
        if (capacity >> percentage) return percentage;
    }
    return -1;
}

struct Presentation
{
    GtkWidget* window; GtkWidget* video; GtkWidget* hud; GtkWidget* status;
    GtkWidget* launch; GtkWidget* logo; GtkWidget* cover;
    std::array<GtkWidget*, 3> progress;
    Decoder& decoder; AudioAndInput& input; lunanx::LunaStreamSession& session;
    std::mutex& status_mutex; std::string& status_line;
    std::shared_ptr<Frame> frame;
    lunanx::PerformanceOverlay overlay;
    Clock::time_point hud_at{};
    std::uint64_t displayed = 0;
    bool updated = false;
    int launch_stage = 1, launch_pulse = 0;
    Clock::time_point launch_began = Clock::now();
    GtkWidget* cancel = nullptr;
    std::atomic<bool> launching{true};
    // « Êtes-vous sûr ? » of the launch screen, and the controller's answer
    // to it: 1 for A (Oui), 2 for B (Non, or the question when none is shown).
    GtkWidget* confirm = nullptr;
    std::atomic<int> launch_answer{0};
};

// « Oui » leaves, « Non » (the default) stays on the launch screen.
void ConfirmCancel(Presentation& player)
{
    if (player.confirm || !player.window || !player.launching.load()) return;
    auto* dialog = gtk_message_dialog_new(GTK_WINDOW(player.window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "%s", lunanx::Tr("Annuler le lancement du jeu").c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
        (lunanx::Tr("Êtes-vous sûr ?") + "\n\n" + lunanx::TrFormat("{} : Oui    {} : Non", {"A", "B"})).c_str());
    gtk_dialog_add_button(GTK_DIALOG(dialog), lunanx::Tr("Non").c_str(), GTK_RESPONSE_NO);
    gtk_dialog_add_button(GTK_DIALOG(dialog), lunanx::Tr("Oui").c_str(), GTK_RESPONSE_YES);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
    g_signal_connect(dialog, "destroy", G_CALLBACK((+[](GtkWidget*, gpointer data) {
        static_cast<Presentation*>(data)->confirm = nullptr;
    })), &player);
    g_signal_connect(dialog, "response", G_CALLBACK((+[](GtkDialog* self, int response, gpointer data) {
        auto& player = *static_cast<Presentation*>(data);
        gtk_widget_destroy(GTK_WIDGET(self));
        if (response == GTK_RESPONSE_YES && player.window) gtk_widget_destroy(player.window);
    })), &player);
    player.confirm = dialog;
    gtk_widget_show_all(dialog);
}
}

void Play(GtkWindow* parent, const lunanx::luna::LunaClient& client, lunanx::luna::Game game)
{
    // The automatic mode's values when it is on.
    const auto settings = lunanx::ResolvedPlaybackPreferences(lunanx::LoadPlaybackPreferences());
    Decoder decoder(settings.fps);
    AudioAndInput input(settings);
    std::mutex status_mutex;
    std::string status_line;
    Pic launch_cover;
    auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), ("LunaONE · " + game.title).c_str());
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 576);
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    auto overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), overlay);
    auto canvas = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(overlay), canvas);
    auto logo_path = std::unique_ptr<gchar, decltype(&g_free)>(g_file_read_link("/proc/self/exe", nullptr), &g_free);
    auto logo = gtk_image_new_from_icon_name("lunaone", GTK_ICON_SIZE_DIALOG);
    if (logo_path)
    {
        const auto path = std::filesystem::path(logo_path.get()).parent_path() / "LunaONE.png";
        if (auto image = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 46, 46, TRUE, nullptr))
        {
            gtk_image_set_from_pixbuf(GTK_IMAGE(logo), image);
            g_object_unref(image);
        }
    }
    gtk_widget_set_halign(logo, GTK_ALIGN_START);
    gtk_widget_set_valign(logo, GTK_ALIGN_START);
    gtk_widget_set_margin_start(logo, 20);
    gtk_widget_set_margin_top(logo, 16);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), logo);
    auto loading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), loading);
    auto cover = gtk_image_new();
    gtk_widget_set_size_request(cover, 146, 192);
    gtk_box_pack_start(GTK_BOX(loading), cover, FALSE, FALSE, 0);
    auto name = gtk_label_new(game.title.c_str());
    gtk_box_pack_start(GTK_BOX(loading), name, FALSE, FALSE, 0);
    auto status = gtk_label_new(lunanx::LaunchStatusText({}, 1).c_str());
    gtk_box_pack_start(GTK_BOX(loading), status, FALSE, FALSE, 0);
    auto progress_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(progress_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(loading), progress_row, FALSE, FALSE, 0);
    std::array<GtkWidget*, 3> progress;
    for (std::size_t i = 0; i < progress.size(); ++i)
    {
        progress[i] = gtk_label_new("■");
        gtk_widget_set_opacity(progress[i], i == 0 ? 1 : .25);
        gtk_box_pack_start(GTK_BOX(progress_row), progress[i], FALSE, FALSE, 0);
    }
    std::jthread artwork([&status_mutex, &launch_cover,
        url = game.image_portrait.empty() ? game.image_landscape : game.image_portrait] {
        try
        {
            if (auto downloaded = Artwork(url, 146, 192))
            {
                std::lock_guard lock(status_mutex);
                launch_cover = std::move(downloaded);
            }
        }
        catch (const std::exception&) {}
    });
    auto hud = gtk_label_new("");
    gtk_widget_set_halign(hud, GTK_ALIGN_END); gtk_widget_set_valign(hud, GTK_ALIGN_START);
    for (auto widget : {hud})
    {
        gtk_widget_set_margin_start(widget, 12);
        gtk_widget_set_margin_end(widget, 12);
        gtk_widget_set_margin_top(widget, 12);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), widget);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(hud), "hud");
    gtk_style_context_add_class(gtk_widget_get_style_context(status), "hud");
    lunanx::LunaStreamSession session(client, std::move(game), {
        [&](auto bytes, auto) { return decoder.Submit(bytes); },
        [&](auto bytes, auto sequence) { input.Submit(bytes, sequence); },
        [&](const auto& text) { std::lock_guard lock(status_mutex); status_line = text; }});
    Presentation state{window, canvas, hud, status, loading, logo, cover, progress, decoder, input, session,
                       status_mutex, status_line, {}, lunanx::PerformanceOverlay(settings.diagnostics)};
    // Bottom right of the launch screen: back to the catalog before the game starts.
    auto cancel_launch = gtk_button_new_with_label(lunanx::Tr("Annuler le lancement du jeu").c_str());
    gtk_widget_set_halign(cancel_launch, GTK_ALIGN_END);
    gtk_widget_set_valign(cancel_launch, GTK_ALIGN_END);
    gtk_widget_set_margin_end(cancel_launch, 28);
    gtk_widget_set_margin_bottom(cancel_launch, 28);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cancel_launch);
    g_signal_connect(cancel_launch, "clicked", G_CALLBACK((+[](GtkButton*, gpointer data) {
        ConfirmCancel(*static_cast<Presentation*>(data));
    })), &state);
    state.cancel = cancel_launch;
    std::atomic<bool> exit_requested{false};
    struct LaunchTimer { Presentation& player; Pic& folded_cover; std::atomic<bool>& exit_requested; };
    LaunchTimer timer_data{state, launch_cover, exit_requested};
    const auto on_close = g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        static_cast<Presentation*>(data)->window = nullptr;
        gtk_main_quit();
    }), &state);
    // F11 switches the borderless full screen; Esc leaves it, and in a window
    // ends the game. The choice is remembered for the next game.
    g_signal_connect(window, "key-press-event", G_CALLBACK((+[](GtkWidget* widget, GdkEventKey* event, gpointer) -> gboolean {
        const auto state = gdk_window_get_state(gtk_widget_get_window(widget));
        const bool fullscreen = (state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
        if (event->keyval == GDK_KEY_F11 || (event->keyval == GDK_KEY_Escape && fullscreen))
        {
            if (fullscreen) gtk_window_unfullscreen(GTK_WINDOW(widget));
            else gtk_window_fullscreen(GTK_WINDOW(widget));
            auto preferences = lunanx::LoadPlaybackPreferences();
            preferences.fullscreen = !fullscreen;
            lunanx::SavePlaybackPreferences(preferences);
            return TRUE;
        }
        if (event->keyval != GDK_KEY_Escape) return FALSE;
        gtk_widget_destroy(widget);
        return TRUE;
    })), nullptr);
    if (settings.fullscreen) gtk_window_fullscreen(GTK_WINDOW(window));
    g_signal_connect(canvas, "draw", G_CALLBACK((+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
        auto& player = *static_cast<Presentation*>(data);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        const auto& frame = player.frame;
        if (!frame) return FALSE;
        const double width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
        const double factor = std::min(width / frame->width, height / frame->height);
        cairo_translate(cr, (width - factor * frame->width) / 2, (height - factor * frame->height) / 2);
        cairo_scale(cr, factor, factor);
        auto surface = cairo_image_surface_create_for_data(frame->pixels.data(), CAIRO_FORMAT_RGB24,
                                                            frame->width, frame->height, frame->width * 4);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
        if (std::exchange(player.updated, false)) ++player.displayed;
        return FALSE;
    })), &state);
    gtk_widget_show_all(window);
    gtk_widget_hide(hud);
    session.Start();
    // SDL offers no change callback for controller state. Read it on a thread
    // of its own every 2 ms rather than from the GTK timer, which also draws.
    // Declared after the session and SDL owner so that it stops before them.
    std::jthread controller([&input, &session, &state, &exit_requested](std::stop_token stop) {
        std::uint32_t previous = 0;
        while (!stop.stop_requested())
        {
            const auto pad = input.Read();
            const std::uint32_t pressed = pad.buttons & ~previous;
            previous = pad.buttons;
            if (lunanx::input::ExitStreamChord(pad))
            {
                exit_requested = true;
                return;
            }
            // Launch screen: B asks « Êtes-vous sûr ? », where A answers Oui
            // and B Non. The GTK timer acts on it.
            if (state.launching.load() && (pressed & (lunanx::input::kA | lunanx::input::kB)))
                state.launch_answer = (pressed & lunanx::input::kA) ? 1 : 2;
            session.SetGamepad(state.overlay.Update(pad));
            std::this_thread::sleep_for(2ms);
        }
    });
    // Picks up each decoded picture within 2 ms (8 ms before).
    const auto timer = g_timeout_add(2, +[](gpointer data) -> gboolean {
        auto& launch = *static_cast<LaunchTimer*>(data);
        auto& player = launch.player;
        if (player.session.Finished())
        {
            gtk_main_quit();
            return G_SOURCE_CONTINUE;
        }
        if (const int answer = player.launch_answer.exchange(0); answer && player.launching.load())
        {
            if (player.confirm)
                gtk_dialog_response(GTK_DIALOG(player.confirm), answer == 1 ? GTK_RESPONSE_YES : GTK_RESPONSE_NO);
            else if (answer == 2)
                ConfirmCancel(player);
            if (!player.window) return G_SOURCE_CONTINUE;
        }
        if (auto frame = player.decoder.TakeFrame())
        {
            player.frame = std::move(frame);
            player.updated = true;
            gtk_widget_hide(player.launch);
            gtk_widget_hide(player.logo);
            if (player.cancel) gtk_widget_hide(player.cancel);
            player.launching = false;
            // The game started: a pending confirmation means « Non ».
            if (player.confirm) gtk_dialog_response(GTK_DIALOG(player.confirm), GTK_RESPONSE_NO);
            gtk_widget_queue_draw(player.video);
        }
        else if (!player.frame)
        {
            std::lock_guard lock(player.status_mutex);
            if (launch.folded_cover)
            {
                gtk_image_set_from_pixbuf(GTK_IMAGE(player.cover), launch.folded_cover.get());
                launch.folded_cover.reset();
            }
            if (!player.status_line.empty())
            {
                const int stage = lunanx::LaunchStage(player.status_line);
                if (stage >= player.launch_stage)
                {
                    player.launch_stage = stage;
                    const auto message = lunanx::LaunchStatusText(player.status_line, stage);
                    gtk_label_set_text(GTK_LABEL(player.status), message.c_str());
                }
            }
        }
        if (launch.exit_requested.load())
        {
            gtk_main_quit();
            return G_SOURCE_CONTINUE;
        }
        player.overlay.Sample({player.session.Stats().received_bytes, player.displayed});
        const auto now = Clock::now();
        if (!player.frame)
        {
            const auto pulse = lunanx::LaunchPulseIndex(now - player.launch_began);
            if (pulse != player.launch_pulse)
            {
                player.launch_pulse = pulse;
                for (std::size_t i = 0; i < player.progress.size(); ++i)
                    gtk_widget_set_opacity(player.progress[i], static_cast<int>(i) == pulse ? 1 : .25);
            }
        }
        if (player.overlay.Visible() && (!gtk_widget_get_visible(player.hud) || now >= player.hud_at))
        {
            gtk_label_set_text(GTK_LABEL(player.hud), player.overlay.Format(Network(), Battery(), player.frame ? player.frame->height : 0).c_str());
            player.hud_at = now + 1s;
        }
        gtk_widget_set_visible(player.hud, player.overlay.Visible());
        return G_SOURCE_CONTINUE;
    }, &timer_data);
    gtk_main();
    g_source_remove(timer);
    controller.request_stop();
    controller.join();
    if (state.window)
    {
        g_signal_handler_disconnect(state.window, on_close);
        gtk_widget_destroy(state.window);
    }
    session.Stop(true);
    if (const auto error = session.Error(); !error.empty())
        throw std::runtime_error(error);
}
}
