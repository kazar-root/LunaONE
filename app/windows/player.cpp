// SPDX-License-Identifier: GPL-3.0-only

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "player.hpp"
#include "fullscreen.hpp"
#include "../desktop/sdl_controllers.hpp"
#include "video_decoder.hpp"
#include "artwork.hpp"
#include "stream/launch_status.hpp"
#include "core/runtime_journal.hpp"
#include "webrtc/luna_stream_session.hpp"
#include "stream/performance_overlay.hpp"
#include "core/playback_preferences.hpp"
#include "core/i18n.hpp"
#include "stream/audio/AudioLatencyPolicy.hpp"
#include "stream/audio/OpusRecovery.hpp"

#include <mmsystem.h>
#include <windowsx.h>
#include <opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <objbase.h>
#include <stdexcept>
#include <utility>

namespace lunaone::windows
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr wchar_t player_class[] = L"LunaONE.PlayerWindow";
constexpr UINT new_frame = WM_APP + 5;
constexpr UINT playback_ended = WM_APP + 6;
constexpr UINT exit_requested = WM_APP + 7;
// B on the launch screen: « Êtes-vous sûr ? ».
constexpr UINT confirm_cancel = WM_APP + 8;

std::wstring Wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = static_cast<int>(text.size());
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    std::wstring result(chars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, result.data(), chars);
    return result;
}

std::string Connection()
{
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(0x01010101); // Route lookup only.
    DWORD index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &index) != NO_ERROR)
        return {};
    MIB_IF_ROW2 active{};
    active.InterfaceIndex = index;
    if (GetIfEntry2(&active) != NO_ERROR)
        return {};
    switch (active.Type)
    {
    case IF_TYPE_IEEE80211: return "Wi-Fi";
    case IF_TYPE_ETHERNET_CSMACD: return "LAN";
    case IF_TYPE_WWANPP: case IF_TYPE_WWANPP2: return "Mobile";
    default: return {};
    }
}

class Audio
{
  public:
    Audio() : settings_(lunanx::ResolvedPlaybackPreferences(lunanx::LoadPlaybackPreferences())) {}

    ~Audio()
    {
        if (output_)
        {
            waveOutReset(output_);
            for (auto& block : blocks_)
                if (block.header.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(output_, &block.header, sizeof(WAVEHDR));
            waveOutClose(output_);
        }
        if (decoder_)
            opus_decoder_destroy(decoder_);
    }

    void Submit(std::span<const std::uint8_t> bytes, std::uint16_t sequence)
    {
        if (!settings_.audio || error_.load())
            return;
        if (!output_ && !Open())
            return;
        const int frames = recovery_.Decode(decoder_, bytes, sequence, pcm_);
        if (frames <= 0)
            return;
        if (running_ && std::ranges::all_of(blocks_, [](const auto& block) { return block.free.load(); }))
        {
            if (waveOutPause(output_) != MMSYSERR_NOERROR)
            {
                error_ = true;
                return;
            }
            running_ = false;
            underrun_ = true;
            primed_ = 0;
        }
        // Latency that persists would keep the sound behind the picture for
        // the rest of the session, so skip packets until the queue is back
        // near its target. Only when the lowest level of the last second shows
        // it: a burst after a network stall is needed to bridge the next one.
        std::size_t queued = 0;
        for (const auto& block : blocks_)
            if (!block.free.load())
                queued += block.header.dwBufferLength / sizeof(std::int16_t);
        const int target = lunanx::audio::StereoSamples(settings_.buffer_ms);
        const auto now = Clock::now();
        if (!running_)
        {
            draining_ = false;
            low_water_ = std::numeric_limits<std::size_t>::max();
            window_start_ = now;
        }
        else
        {
            low_water_ = std::min(low_water_, queued);
            if (now - window_start_ >= 1s)
            {
                draining_ = low_water_ > static_cast<std::size_t>(target + lunanx::audio::StereoSamples(40));
                low_water_ = std::numeric_limits<std::size_t>::max();
                window_start_ = now;
            }
        }
        if (draining_)
        {
            if (queued > static_cast<std::size_t>(target + lunanx::audio::StereoSamples(20)))
                return;
            draining_ = false;
        }
        for (auto& block : blocks_)
        {
            if (!block.free.exchange(false))
                continue;
            const auto samples = static_cast<std::size_t>(frames) * 2;
            lunanx::audio::ApplyVolume({pcm_.data(), samples}, settings_.volume);
            std::copy_n(pcm_.data(), samples, block.samples.data());
            block.header.dwBufferLength = static_cast<DWORD>(samples * sizeof(std::int16_t));
            if (waveOutWrite(output_, &block.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            {
                block.free = true;
                error_ = true;
            }
            else if (!running_)
            {
                primed_ += samples;
                // After an underrun, half the target is enough to resume:
                // waiting for all of it added that much silence to each hiccup.
                const int resume = underrun_ ? std::max(target / 2, lunanx::audio::StereoSamples(10)) : target;
                if (primed_ >= resume)
                {
                    running_ = true;
                    if (waveOutRestart(output_) != MMSYSERR_NOERROR) error_ = true;
                }
            }
            return;
        }
    }

    bool Failed() const { return error_.load(); }

  private:
    using Samples = std::array<std::int16_t, 5760 * 2>;
    struct Block
    {
        Samples samples{};
        WAVEHDR header{};
        std::atomic_bool free = true;
    };

    static void CALLBACK OutputCallback(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR value, DWORD_PTR)
    {
        if (message == WOM_DONE)
        {
            const auto header = reinterpret_cast<WAVEHDR*>(value);
            reinterpret_cast<Block*>(header->dwUser)->free = true;
        }
    }

    bool Open()
    {
        decoder_ = opus_decoder_create(48000, 2, nullptr);
        constexpr WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
        if (!decoder_ || waveOutOpen(&output_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(OutputCallback),
                                     0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        {
            error_ = true;
            return false;
        }
        if (waveOutPause(output_) != MMSYSERR_NOERROR)
        {
            error_ = true;
            return false;
        }
        for (auto& block : blocks_)
        {
            block.header.lpData = reinterpret_cast<LPSTR>(block.samples.data());
            block.header.dwBufferLength = static_cast<DWORD>(block.samples.size() * sizeof(std::int16_t));
            block.header.dwUser = reinterpret_cast<DWORD_PTR>(&block);
            if (waveOutPrepareHeader(output_, &block.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            {
                error_ = true;
                return false;
            }
        }
        return true;
    }

    OpusDecoder* decoder_{};
    HWAVEOUT output_{};
    const lunanx::PlaybackPreferences settings_;
    bool running_ = false;
    bool underrun_ = false;
    bool draining_ = false;
    std::size_t low_water_ = std::numeric_limits<std::size_t>::max();
    Clock::time_point window_start_{};
    int primed_ = 0;
    std::array<Block, 14> blocks_{};
    Samples pcm_{};
    lunanx::audio::OpusRecovery recovery_;
    std::atomic_bool error_ = false;
};

class Player
{
  public:
    Player(HWND owner, const lunanx::luna::LunaClient& client, lunanx::luna::Game game)
        : owner_(owner), title_(L"LunaONE · " + Wide(game.title)),
          game_title_(Wide(game.title)), cover_url_(game.image_portrait.empty() ? game.image_landscape : game.image_portrait),
          session_(client, std::move(game), {
              [this](auto bytes, auto time) { return video_->Submit(bytes, time); },
              [this](auto bytes, auto sequence) { audio_.Submit(bytes, sequence); },
              [this](const std::string& status) {
                  lunanx::LogRuntimeEvent("windows", "stream_status", status);
                  std::lock_guard lock(status_mutex_);
                  const auto stage = lunanx::LaunchStage(status);
                  if (stage < loading_stage_) return;
                  loading_stage_ = stage;
                  status_ = Wide(lunanx::LaunchStatusText(status, stage));
              }})
    {
    }

    ~Player()
    {
        StopInput();
        if (cleanup_.joinable())
            cleanup_.join();
        else if (started_)
            session_.Stop(true);
        video_.reset();
        if (window_)
            DestroyWindow(window_);
        if (IsWindow(owner_))
            EnableWindow(owner_, TRUE);
    }

    void Run()
    {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = player_class;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Impossible d’enregistrer la fenêtre du lecteur.");
        if (!CreateWindowExW(0, player_class, title_.c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1040, 620, owner_, nullptr, instance, this))
            throw std::runtime_error("Impossible d’ouvrir le lecteur LunaONE.");

        if (!cover_url_.empty())
            cover_download_ = std::async(std::launch::async, [address = cover_url_] {
                if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return Bitmap{};
                Bitmap bitmap;
                try { bitmap.reset(LoadArtwork(address, 146, 192), [](auto image) { if (image) DeleteObject(image); }); }
                catch (const std::exception&) {}
                CoUninitialize();
                return bitmap;
            });
        video_ = std::make_unique<VideoDecoder>([this, target = window_](std::shared_ptr<const VideoFrame> frame) {
            bool notify;
            {
                std::lock_guard lock(frame_mutex_);
                notify = !next_frame_;
                next_frame_ = std::move(frame);
            }
            if (notify)
                PostMessageW(target, new_frame, 0, 0);
        }, lunanx::ResolvedPlaybackPreferences(lunanx::LoadPlaybackPreferences()).fps);
        if (!SetTimer(window_, 1, 10, nullptr))
            throw std::runtime_error("Impossible de démarrer la lecture LunaONE.");
        EnableWindow(owner_, FALSE);
        ShowWindow(window_, SW_SHOW);
        if (lunanx::LoadPlaybackPreferences().fullscreen) SetFullscreen(true);
        SetForegroundWindow(window_);
        MSG message{};
        while (window_ && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (message.message == WM_QUIT)
            PostQuitMessage(static_cast<int>(message.wParam));
        if (!error_.empty())
            throw std::runtime_error(error_);
    }

  private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            const auto self = static_cast<Player*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Player*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);

        try
        {
            switch (message)
            {
            case WM_PAINT: self->Paint(); return 0;
            case WM_LBUTTONDOWN:
            {
                // « Annuler le lancement du jeu », bottom right of the launch screen.
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (self->launching_.load() && PtInRect(&self->cancel_rect_, point))
                {
                    self->ConfirmCancel();
                    return 0;
                }
                break;
            }
            case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_TIMER: self->Tick(); return 0;
            case WM_CLOSE: self->Finish(); return 0;
            case WM_KEYDOWN:
                // F11 switches full screen; Esc leaves it, and in a window ends the game.
                if (wparam == VK_F11)
                {
                    self->ToggleFullscreen();
                    return 0;
                }
                if (wparam == VK_ESCAPE)
                {
                    if (self->fullscreen_) self->ToggleFullscreen();
                    else self->Finish();
                    return 0;
                }
                break;
            case WM_SYSKEYDOWN:
                if (wparam == VK_RETURN)
                {
                    self->ToggleFullscreen();
                    return 0;
                }
                break;
            case WM_SYSCOMMAND:
                if (IsMaximizeCommand(wparam))
                {
                    if (!self->fullscreen_) self->ToggleFullscreen();
                    return 0;
                }
                break;
            case new_frame: self->Present(); return 0;
            case exit_requested: self->Finish(); return 0;
            case confirm_cancel: self->ConfirmCancel(); return 0;
            case playback_ended:
                self->cleanup_.join();
                DestroyWindow(window);
                return 0;
            case WM_NCDESTROY:
                self->window_ = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            }
        }
        catch (const std::exception& error)
        {
            self->error_ = error.what();
            self->Finish();
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void SetFullscreen(bool fullscreen)
    {
        if (fullscreen == fullscreen_ || !window_) return;
        SetBorderlessFullscreen(window_, fullscreen, placement_);
        fullscreen_ = fullscreen;
    }

    void ToggleFullscreen()
    {
        SetFullscreen(!fullscreen_);
        auto preferences = lunanx::LoadPlaybackPreferences();
        preferences.fullscreen = fullscreen_;
        lunanx::SavePlaybackPreferences(preferences);
    }

    void Tick()
    {
        if (stopping_)
            return;
        if (auto error = video_->Error(); !error.empty())
        {
            error_ = std::move(error);
            Finish();
            return;
        }
        const auto now = Clock::now();
        if (!started_)
        {
            if (!video_->Ready())
                return;
            session_.Start();
            started_ = true;
            began_ = now;
            StartInput();
        }
        if (session_.Finished())
        {
            error_ = session_.Error();
            Finish();
            return;
        }
        if (displayed_ == 0)
        {
            // Provisioning can take minutes. Time the decoder only once video is arriving.
            if (video_arrival_ == Clock::time_point{} && session_.Stats().video_units > 0)
                video_arrival_ = now;
            if (video_arrival_ != Clock::time_point{} && now - video_arrival_ > 12s)
            {
                error_ = "Le flux vidéo est reçu, mais le décodeur Windows n’a affiché aucune image.";
                Finish();
                return;
            }
            if (cover_download_.valid() && cover_download_.wait_for(0s) == std::future_status::ready)
            {
                cover_ = cover_download_.get();
                InvalidateRect(window_, nullptr, FALSE);
            }
            std::scoped_lock lock(status_mutex_);
            const auto pulse = lunanx::LaunchPulseIndex(now - began_);
            if (paint_status_ != status_ || paint_pulse_ != pulse)
            {
                paint_status_ = status_;
                paint_pulse_ = pulse;
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
        overlay_.Sample({session_.Stats().received_bytes, displayed_});
        if (overlay_.Visible() && (hud_.empty() || now >= next_hud_))
        {
            SYSTEM_POWER_STATUS power{};
            const int battery = GetSystemPowerStatus(&power) && power.BatteryFlag != 128 &&
                power.BatteryLifePercent <= 100 ? power.BatteryLifePercent : -1;
            hud_ = Wide(overlay_.Format(Connection(), battery, displayed_frame_ ? displayed_frame_->height : 0));
            next_hud_ = now + 1s;
            InvalidateRect(window_, nullptr, FALSE);
        }
        else if (!overlay_.Visible() && !hud_.empty())
        {
            hud_.clear();
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (audio_.Failed() && !audio_warning_)
        {
            audio_warning_ = true;
            SetWindowTextW(window_, (title_ + L" · Sortie audio indisponible").c_str());
        }
    }

    // The pad state has no change notification. Poll it on a thread of its
    // own every 2 ms: WM_TIMER ticks every 15.6 ms at best and is only
    // delivered once every other message, including each new picture, has
    // been handled. SDL reads Xbox, PlayStation, Nintendo and 8BitDo pads, over
    // USB or Bluetooth, and belongs to this thread for the whole game.
    void StartInput()
    {
        input_running_ = true;
        input_ = std::thread([this, target = window_] {
            const bool fine_timer = timeBeginPeriod(1) == TIMERR_NOERROR;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            lunaone::desktop::SdlControllers controllers;
            std::uint32_t previous_buttons = 0;
            while (input_running_.load())
            {
                lunanx::input::GamepadState pad;
                if (controllers.Read(pad))
                {
                    controller_seen_ = true;
                    family_ = controllers.Family();
                }
                // The confirmation box of the launch screen belongs to the window too.
                const HWND box = GetWindow(target, GW_ENABLEDPOPUP);
                const HWND foreground = GetForegroundWindow();
                if (foreground == target || (box && box != target && foreground == box))
                {
                    const std::uint32_t pressed = pad.buttons & ~previous_buttons;
                    previous_buttons = pad.buttons;
                    if (lunanx::input::ExitStreamChord(pad))
                    {
                        PostMessageW(target, exit_requested, 0, 0);
                        break;
                    }
                    // Launch screen: B asks « Êtes-vous sûr ? », where A answers Oui and B Non.
                    if (launching_.load() && (pressed & (lunanx::input::kA | lunanx::input::kB)))
                    {
                        if (box && box != target)
                            PostMessageW(box, WM_COMMAND, (pressed & lunanx::input::kA) ? IDYES : IDNO, 0);
                        else if (pressed & lunanx::input::kB)
                            PostMessageW(target, confirm_cancel, 0, 0);
                    }
                    session_.SetGamepad(overlay_.Update(pad));
                }
                else
                {
                    session_.SetGamepad(overlay_.Update({}));
                }
                std::this_thread::sleep_for(2ms);
            }
            if (fine_timer)
                timeEndPeriod(1);
        });
    }

    void StopInput()
    {
        input_running_ = false;
        if (input_.joinable())
            input_.join();
    }

    // « Oui » leaves, « Non » (the default) stays on the launch screen. The
    // controller answers too: A Oui, B Non.
    void ConfirmCancel()
    {
        if (confirming_ || !launching_.load()) return;
        confirming_ = true;
        const auto title = Wide(lunanx::Tr("Annuler le lancement du jeu"));
        const auto question = Wide(lunanx::Tr("Êtes-vous sûr ?") + "\n\n" +
                                   lunanx::TrFormat("{} : Oui    {} : Non",
                                       {lunanx::input::PadButtonName(family_, lunanx::input::PadButton::A),
                                        lunanx::input::PadButtonName(family_, lunanx::input::PadButton::B)}));
        const int answer = MessageBoxW(window_, question.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        confirming_ = false;
        if (answer == IDYES) Finish();
    }

    void Finish()
    {
        if (stopping_)
            return;
        stopping_ = true;
        KillTimer(window_, 1);
        StopInput();
        if (!started_)
        {
            video_.reset();
            DestroyWindow(window_);
            return;
        }
        paint_status_ = L"Fin de la partie…";
        displayed_frame_.reset();
        InvalidateRect(window_, nullptr, FALSE);
        const HWND target = window_;
        try
        {
            cleanup_ = std::thread([this, target] {
                session_.Stop(true);
                video_.reset();
                PostMessageW(target, playback_ended, 0, 0);
            });
        }
        catch (const std::system_error&)
        {
            session_.Stop(true);
            video_.reset();
            DestroyWindow(window_);
        }
        LogMetrics();
    }

    void LogMetrics() noexcept
    {
        try
        {
            const auto counters = session_.Stats();
            lunanx::LogRuntimeEvent("windows", "stream_metrics",
                "displayed=" + std::to_string(displayed_) +
                " first_frame_ms=" + std::to_string(displayed_ ?
                    std::chrono::duration_cast<std::chrono::milliseconds>(first_frame_ - began_).count() : 0) +
                " max_frame_gap_ms=" + std::to_string(max_frame_gap_.count()) +
                " playback_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    last_frame_ - first_frame_).count()) +
                " video_units=" + std::to_string(counters.video_units) +
                " audio_packets=" + std::to_string(counters.audio_packets) +
                " transport_rtt_ms=" + std::to_string(counters.transport_rtt_ms) +
                " gamepad_reports=" + std::to_string(counters.gamepad_reports) +
                " gamepad_changes=" + std::to_string(counters.gamepad_changes) +
                " controller_detected=" + std::to_string(controller_seen_.load()));
        }
        catch (...) {}
    }

    void Present()
    {
        {
            std::lock_guard lock(frame_mutex_);
            if (stopping_)
            {
                next_frame_.reset();
                return;
            }
            displayed_frame_ = std::exchange(next_frame_, {});
            if (displayed_frame_) launching_ = false;
        }
        // The game started: a pending confirmation means « Non ».
        if (confirming_)
            if (const HWND box = GetWindow(window_, GW_ENABLEDPOPUP); box && box != window_)
                PostMessageW(box, WM_COMMAND, IDNO, 0);
        // Paint now: an invalidated window is only painted once the message
        // queue is empty, which held each picture behind any pending message.
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
    }

    void Paint()
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        RECT bounds{};
        GetClientRect(window_, &bounds);
        const auto* frame = displayed_frame_.get();
        if (!frame)
        {
            FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            if (auto logo = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)))
                DrawIconEx(dc, 20, 20, logo, 44, 44, 0, nullptr, DI_NORMAL);
            const int center = bounds.right / 2;
            const int top = std::max(48L, bounds.bottom / 2 - 133);
            if (cover_)
            {
                if (const auto source = CreateCompatibleDC(dc))
                {
                    auto original = SelectObject(source, cover_.get());
                    BitBlt(dc, center - 73, top, 146, 192, source, 0, 0, SRCCOPY);
                    SelectObject(source, original);
                    DeleteDC(source);
                }
            }
            else
            {
                RECT empty{center - 73, top, center + 73, top + 192};
                auto brush = CreateSolidBrush(RGB(32, 23, 57));
                FillRect(dc, &empty, brush);
                DeleteObject(brush);
            }
            RECT name{18, top + 201, bounds.right - 18, top + 226};
            DrawTextW(dc, game_title_.c_str(), -1, &name, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RECT status{18, top + 230, bounds.right - 18, top + 255};
            DrawTextW(dc, paint_status_.c_str(), -1, &status, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            for (int i = 0; i < 3; ++i)
            {
                const int x = center - 22 + i * 17;
                RECT square{x, top + 266, x + 10, top + 276};
                FillRect(dc, &square, static_cast<HBRUSH>(GetStockObject(i == paint_pulse_ ? WHITE_BRUSH : DKGRAY_BRUSH)));
            }
            // Bottom right: back to the catalog before the game starts.
            const auto cancel = Wide(lunanx::Tr("Annuler le lancement du jeu"));
            cancel_rect_ = {bounds.right - 28 - 300, bounds.bottom - 28 - 44, bounds.right - 28, bounds.bottom - 28};
            auto fill = CreateSolidBrush(RGB(24, 20, 43));
            auto edge = CreatePen(PS_SOLID, 1, RGB(125, 113, 142));
            auto old_brush = SelectObject(dc, fill);
            auto old_pen = SelectObject(dc, edge);
            RoundRect(dc, cancel_rect_.left, cancel_rect_.top, cancel_rect_.right, cancel_rect_.bottom, 44, 44);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(fill);
            DeleteObject(edge);
            RECT label = cancel_rect_;
            DrawTextW(dc, cancel.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        else
        {
            const double scale = std::min(static_cast<double>(bounds.right) / frame->width,
                                          static_cast<double>(bounds.bottom) / frame->height);
            const int width = static_cast<int>(frame->width * scale);
            const int height = static_cast<int>(frame->height * scale);
            RECT picture{(bounds.right - width) / 2, (bounds.bottom - height) / 2,
                         (bounds.right - width) / 2 + width, (bounds.bottom - height) / 2 + height};
            const auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            const RECT bars[]{{0, 0, bounds.right, picture.top}, {0, picture.bottom, bounds.right, bounds.bottom},
                              {0, picture.top, picture.left, picture.bottom},
                              {picture.right, picture.top, bounds.right, picture.bottom}};
            for (const auto& bar : bars)
                if (!IsRectEmpty(&bar))
                    FillRect(dc, &bar, black);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = frame->width;
            bitmap.bmiHeader.biHeight = -frame->height;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            const int drawn = StretchDIBits(dc, picture.left, picture.top, width, height, 0, 0,
                frame->width, frame->height, frame->pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            if (drawn > 0 && frame != last_painted_.get())
            {
                const auto now = Clock::now();
                if (!displayed_++)
                    first_frame_ = now;
                else
                    max_frame_gap_ = std::max(max_frame_gap_, std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_));
                last_frame_ = now;
                last_painted_ = displayed_frame_;
            }
        }
        if (!hud_.empty())
        {
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            SIZE size{};
            GetTextExtentPoint32W(dc, hud_.c_str(), static_cast<int>(hud_.size()), &size);
            RECT badge{bounds.right - size.cx - 24, 12, bounds.right - 12, size.cy + 24};
            FillRect(dc, &badge, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, hud_.c_str(), -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(window_, &paint);
    }

    HWND owner_, window_{};
    std::atomic<bool> launching_{true};
    bool confirming_ = false;
    RECT cancel_rect_{};
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    std::wstring title_, game_title_;
    using Bitmap = std::shared_ptr<std::remove_pointer_t<HBITMAP>>;
    std::string cover_url_;
    std::future<Bitmap> cover_download_;
    Bitmap cover_;
    int paint_pulse_ = 0, loading_stage_ = 1;
    std::wstring hud_;
    lunanx::PerformanceOverlay overlay_{lunanx::LoadPlaybackPreferences().diagnostics};
    Clock::time_point next_hud_{};
    std::wstring paint_status_ = Wide(lunanx::LaunchStatusText({}, 1));
    std::mutex status_mutex_, frame_mutex_;
    std::wstring status_ = paint_status_;
    std::shared_ptr<const VideoFrame> next_frame_, displayed_frame_, last_painted_;
    std::unique_ptr<VideoDecoder> video_;
    Audio audio_;
    lunanx::LunaStreamSession session_;
    std::thread cleanup_;
    std::thread input_;
    std::atomic<bool> input_running_{false};
    Clock::time_point began_{}, first_frame_{}, last_frame_{}, video_arrival_{};
    std::chrono::milliseconds max_frame_gap_{};
    std::uint64_t displayed_ = 0;
    std::atomic<bool> controller_seen_{false};
    // The labels of the pad in hand, for the launch screen's question.
    std::atomic<lunanx::input::ControllerFamily> family_{lunanx::input::ControllerFamily::Xbox};
    bool audio_warning_ = false, started_ = false, stopping_ = false;
    std::string error_;
};
} // namespace

void Play(HWND owner, const lunanx::luna::LunaClient& client, lunanx::luna::Game game)
{
    auto player = std::make_unique<Player>(owner, client, std::move(game));
    player->Run();
}
} // namespace lunaone::windows
