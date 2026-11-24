// SPDX-License-Identifier: GPL-3.0-only
// LunaONE for Windows: the shared Google TV library (ui/library_scene) drawn
// with Direct2D and DirectWrite, driven by the mouse, the keyboard or a
// controller read through SDL2. Playback is lunaone::windows::Play.
#include "../desktop/runtime.hpp"
#include "../desktop/sdl_controllers.hpp"
#include "fullscreen.hpp"
#include "player.hpp"
#include "core/http_client.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "luna/amazon_auth.hpp"
#include "ui/library_scene.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace
{
using Microsoft::WRL::ComPtr;
using lunanx::ui::Font;
using lunanx::ui::Paint;
constexpr UINT run_tasks = WM_APP + 1;
constexpr wchar_t window_class[] = L"LunaONE.Library";

std::wstring Wide(std::string_view value)
{
    if (value.empty()) return {};
    const int size = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), size, nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), size, result.data(), length);
    return result;
}

std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), size, nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), size, result.data(), length, nullptr, nullptr);
    return result;
}

D2D1_COLOR_F Color(std::uint32_t argb, float opacity = 1)
{
    return {((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f,
            ((argb >> 24) & 255) / 255.f * opacity};
}

// "Wi-Fi" or "LAN" for the connected adapter with a gateway.
std::string NetworkLabel()
{
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, addresses, &size) != NO_ERROR)
        return {};
    for (auto adapter = addresses; adapter; adapter = adapter->Next)
        if (adapter->OperStatus == IfOperStatusUp && adapter->FirstGatewayAddress)
            return adapter->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi" : "LAN";
    return {};
}

class Desktop final : public lunanx::ui::SceneHost
{
  public:
    Desktop() : scene_(*this) {}

    ~Desktop() override
    {
        {
            std::lock_guard lock(images_mutex_);
            stopping_ = true;
        }
        images_ready_.notify_all();
        if (images_.joinable()) images_.join();
    }

    int Run(HINSTANCE instance, int show)
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(write_.GetAddressOf()))) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()))))
            throw std::runtime_error("Direct2D is unavailable.");
        WNDCLASSW window {};
        window.style = CS_DBLCLKS;
        window.lpfnWndProc = Procedure;
        window.hInstance = instance;
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        window.lpszClassName = window_class;
        if (!RegisterClassW(&window)) throw std::runtime_error("Impossible d’enregistrer la fenêtre LunaONE.");
        const UINT dpi = GetDpiForSystem();
        RECT frame {0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        if (!CreateWindowExW(0, window_class, L"LunaONE", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this))
            throw std::runtime_error("Impossible d’ouvrir la fenêtre LunaONE.");
        ShowWindow(window_, show);
        if (lunanx::LoadPlaybackPreferences().fullscreen) SetFullscreen(true);
        ReportDisplay();
        controllers_.emplace();
        images_ = std::thread([this] { LoadImages(); });
        SetTimer(window_, 1, 16, nullptr);
        scene_.Start();
        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    // SceneHost
    void Post(std::function<void()> task) override
    {
        {
            std::lock_guard lock(tasks_mutex_);
            tasks_.push_back(std::move(task));
        }
        PostMessageW(window_, run_tasks, 0, 0);
    }

    void Redraw() override
    {
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }

    float TextWidth(const std::string& text, float size, Font font) override
    {
        auto layout = Layout(text, size, font, 100000, 1000, false);
        DWRITE_TEXT_METRICS metrics {};
        if (!layout || FAILED(layout->GetMetrics(&metrics))) return text.size() * size * .5f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    float TextHeight(const std::string& text, float size, Font font, float width) override
    {
        auto layout = Layout(text, size, font, width, 100000, true);
        DWRITE_TEXT_METRICS metrics {};
        if (!layout || FAILED(layout->GetMetrics(&metrics))) return size * 1.3f;
        return metrics.height;
    }

    bool ImageReady(const std::string& address) override
    {
        if (decoded_.contains(address)) return decoded_[address] != nullptr;
        if (requested_.insert(address).second)
        {
            {
                std::lock_guard lock(images_mutex_);
                queue_.push_back(address);
            }
            images_ready_.notify_one();
        }
        return false;
    }

    void Play(const lunanx::luna::LunaClient& client, const lunanx::luna::Game& game) override
    {
        // The player reads the controllers on a thread of its own, and SDL
        // serves one thread at a time: the library lends them for the game.
        controllers_.reset();
        struct Reopen
        {
            Desktop* self;
            ~Reopen() { self->controllers_.emplace(); }
        } reopen{this};
        lunaone::windows::Play(window_, client, game);
        SetForegroundWindow(window_);
    }

    bool Fullscreen() override { return fullscreen_; }

    void SetFullscreen(bool fullscreen) override
    {
        if (fullscreen == fullscreen_) return;
        lunaone::windows::SetBorderlessFullscreen(window_, fullscreen, placement_);
        fullscreen_ = fullscreen;
        Redraw();
    }

    void OpenUrl(const std::string& address) override
    {
        ShellExecuteW(window_, L"open", Wide(address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void Quit() override { DestroyWindow(window_); }

    std::string SystemStatus() override
    {
        SYSTEMTIME now {};
        GetLocalTime(&now);
        char clock[8] = {};
        std::snprintf(clock, sizeof(clock), "%02d:%02d", now.wHour, now.wMinute);
        std::string status = clock;
        if (const auto network = NetworkLabel(); !network.empty()) status += " · " + network;
        SYSTEM_POWER_STATUS power {};
        if (GetSystemPowerStatus(&power) && !(power.BatteryFlag & 128) && power.BatteryLifePercent <= 100)
            status += " · " + std::to_string(power.BatteryLifePercent) + "%";
        return status;
    }

  private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            auto self = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto self = reinterpret_cast<Desktop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        try
        {
            switch (message)
            {
            case WM_PAINT: self->PaintWindow(); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_SIZE:
                if (self->target_) self->target_->Resize(D2D1_SIZE_U {LOWORD(lparam), HIWORD(lparam)});
                self->Redraw();
                return 0;
            case WM_DPICHANGED:
            {
                const auto& suggested = *reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                             suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->target_.Reset();
                self->bitmaps_.clear();
                return 0;
            }
            case WM_GETMINMAXINFO:
            {
                auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
                const UINT dpi = GetDpiForWindow(window);
                info.ptMinTrackSize = {MulDiv(960, dpi, 96), MulDiv(560, dpi, 96)};
                return 0;
            }
            case WM_TIMER: self->Tick(); return 0;
            case run_tasks: self->RunTasks(); return 0;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            {
                const float scale = 96.f / GetDpiForWindow(window);
                self->scene_.Click(GET_X_LPARAM(lparam) * scale, GET_Y_LPARAM(lparam) * scale,
                                   message == WM_LBUTTONDBLCLK ? 2 : 1);
                return 0;
            }
            case WM_MOUSEMOVE: self->scene_.PointerMoved(); return 0;
            case WM_MOUSEWHEEL:
                self->scene_.Wheel(0, GET_WHEEL_DELTA_WPARAM(wparam) / 3.f);
                return 0;
            case WM_MOUSEHWHEEL:
                self->scene_.Wheel(-GET_WHEEL_DELTA_WPARAM(wparam) / 3.f, 0);
                return 0;
            case WM_SYSCOMMAND:
                if (lunaone::windows::IsMaximizeCommand(wparam))
                {
                    if (!self->fullscreen_) self->scene_.ToggleFullscreen();
                    return 0;
                }
                break;
            case WM_SYSKEYDOWN:
                // Alt + Enter switches full screen, as in Windows games.
                if (wparam == VK_RETURN)
                {
                    self->scene_.ToggleFullscreen();
                    return 0;
                }
                break;
            case WM_KEYDOWN:
                if (self->Key(wparam)) return 0;
                break;
            case WM_CHAR:
                if (self->scene_.Typing() && wparam >= 0x20 && wparam != 0x7f)
                {
                    const wchar_t character[2] = {static_cast<wchar_t>(wparam), 0};
                    self->scene_.TypeText(Utf8(character));
                }
                return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            }
        }
        catch (const std::exception& error)
        {
            MessageBoxW(window, Wide(error.what()).c_str(), L"LunaONE", MB_OK | MB_ICONERROR);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    bool Key(WPARAM key)
    {
        using lunanx::ui::InputDevice;
        using lunanx::ui::Move;
        switch (key)
        {
        case VK_F11: scene_.ToggleFullscreen(); return true;
        case VK_ESCAPE:
            // Esc closes a panel, otherwise leaves full screen for the window.
            if (!scene_.Escape() && fullscreen_) scene_.ToggleFullscreen();
            return true;
        case VK_BACK: scene_.Backspace(); return true;
        case VK_LEFT: scene_.Navigate(Move::Left, InputDevice::Keyboard); return true;
        case VK_RIGHT: scene_.Navigate(Move::Right, InputDevice::Keyboard); return true;
        case VK_UP: scene_.Navigate(Move::Up, InputDevice::Keyboard); return true;
        case VK_DOWN: scene_.Navigate(Move::Down, InputDevice::Keyboard); return true;
        case VK_RETURN: scene_.Navigate(Move::Activate, InputDevice::Keyboard); return true;
        case VK_SPACE:
            if (scene_.Typing()) return false;
            scene_.Navigate(Move::Activate, InputDevice::Keyboard);
            return true;
        default: return false;
        }
    }

    void RunTasks()
    {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lock(tasks_mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) task();
    }

    void Tick()
    {
        // Read even in the background, so that connections and disconnections
        // are seen; act only while the library is in front.
        lunanx::input::GamepadState pad;
        if (controllers_ && controllers_->Read(pad) && GetForegroundWindow() == window_)
            scene_.Gamepad(&pad, controllers_->Family());
        else
            scene_.Gamepad(nullptr, lunanx::input::ControllerFamily::Xbox);
        const auto now = std::chrono::steady_clock::now();
        if (scene_.Animating() || now >= next_clock_)
        {
            if (now >= next_clock_) next_clock_ = now + std::chrono::seconds(30);
            Redraw();
        }
    }

    void ReportDisplay()
    {
        MONITORINFOEXW monitor {};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        const int refresh = EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 0;
        lunanx::SetDeviceDisplay({monitor.rcMonitor.bottom - monitor.rcMonitor.top, refresh});
    }

    ComPtr<IDWriteTextFormat> Format(float size, Font font)
    {
        const auto key = std::make_pair(static_cast<int>(size * 4), static_cast<int>(font));
        if (auto found = formats_.find(key); found != formats_.end()) return found->second;
        ComPtr<IDWriteTextFormat> format;
        write_->CreateTextFormat(font == Font::Mono ? L"Consolas" : L"Segoe UI", nullptr,
                                 font == Font::Regular ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", format.GetAddressOf());
        formats_[key] = format;
        return format;
    }

    ComPtr<IDWriteTextLayout> Layout(const std::string& text, float size, Font font, float width, float height, bool wrap)
    {
        auto format = Format(size, font);
        if (!format) return {};
        const auto wide = Wide(text);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(write_->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()), format.Get(),
                                            std::max(1.f, width), std::max(1.f, height), layout.GetAddressOf())))
            return {};
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap)
        {
            // One line ending with an ellipsis, as on the TV.
            DWRITE_TRIMMING trimming {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            write_->CreateEllipsisTrimmingSign(format.Get(), ellipsis.GetAddressOf());
            layout->SetTrimming(&trimming, ellipsis.Get());
        }
        return layout;
    }

    void PaintWindow()
    {
        PAINTSTRUCT paint;
        BeginPaint(window_, &paint);
        EndPaint(window_, &paint);
        RECT client;
        GetClientRect(window_, &client);
        if (!target_)
        {
            const UINT dpi = GetDpiForWindow(window_);
            const auto properties = D2D1_RENDER_TARGET_PROPERTIES {D2D1_RENDER_TARGET_TYPE_DEFAULT,
                {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN}, static_cast<float>(dpi), static_cast<float>(dpi),
                D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
            const auto window = D2D1_HWND_RENDER_TARGET_PROPERTIES {window_,
                {static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)}, D2D1_PRESENT_OPTIONS_NONE};
            if (FAILED(factory_->CreateHwndRenderTarget(properties, window, target_.GetAddressOf()))) return;
            target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            bitmaps_.clear();
        }
        const auto size = target_->GetSize();
        const auto paints = scene_.Draw(size.width, size.height);
        target_->BeginDraw();
        target_->Clear(Color(0xff100d1d));
        ComPtr<ID2D1SolidColorBrush> brush;
        target_->CreateSolidColorBrush(Color(0xffffffff), brush.GetAddressOf());
        for (const auto& item : paints) Draw(item, brush.Get());
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            target_.Reset();
            bitmaps_.clear();
            Redraw();
        }
    }

    void Draw(const Paint& paint, ID2D1SolidColorBrush* brush)
    {
        const auto& box = paint.box;
        const D2D1_RECT_F rect {box.x, box.y, box.x + box.w, box.y + box.h};
        switch (paint.kind)
        {
        case Paint::Kind::Rect:
        {
            const D2D1_ROUNDED_RECT rounded {rect, paint.radius, paint.radius};
            if ((paint.color >> 24) != 0)
            {
                brush->SetColor(Color(paint.color, paint.opacity));
                target_->FillRoundedRectangle(rounded, brush);
            }
            if (paint.stroke > 0)
            {
                const float inset = paint.stroke / 2;
                const D2D1_ROUNDED_RECT edge {{rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset},
                                              std::max(0.f, paint.radius - inset), std::max(0.f, paint.radius - inset)};
                brush->SetColor(Color(paint.stroke_color, paint.opacity));
                target_->DrawRoundedRectangle(edge, brush, paint.stroke);
            }
            break;
        }
        case Paint::Kind::Text:
        {
            auto layout = Layout(paint.text, paint.size, paint.font, box.w, paint.wrap ? 100000 : box.h, paint.wrap);
            if (!layout) break;
            layout->SetTextAlignment(paint.align == lunanx::ui::Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER :
                                     paint.align == lunanx::ui::Align::Right  ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TEXT_METRICS metrics {};
            layout->GetMetrics(&metrics);
            const float top = paint.middle ? box.y + (box.h - metrics.height) / 2 : box.y;
            brush->SetColor(Color(paint.color, paint.opacity));
            target_->PushAxisAlignedClip({rect.left - 2, rect.top - 2, rect.right + 2, rect.bottom + 2},
                                         D2D1_ANTIALIAS_MODE_ALIASED);
            target_->DrawTextLayout({box.x, top}, layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target_->PopAxisAlignedClip();
            break;
        }
        case Paint::Kind::Image:
        {
            auto bitmap = Bitmap(paint.text);
            if (!bitmap) break;
            const auto source = bitmap->GetSize();
            const float scale = paint.fit ? std::min(box.w / source.width, box.h / source.height)
                                          : std::max(box.w / source.width, box.h / source.height);
            const float w = source.width * scale, h = source.height * scale;
            const D2D1_RECT_F destination {box.x + (box.w - w) / 2, box.y + (box.h - h) / 2,
                                           box.x + (box.w + w) / 2, box.y + (box.h + h) / 2};
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            factory_->CreateRoundedRectangleGeometry({rect, paint.radius, paint.radius}, clip.GetAddressOf());
            ComPtr<ID2D1Layer> layer;
            target_->CreateLayer(nullptr, layer.GetAddressOf());
            D2D1_LAYER_PARAMETERS parameters {rect, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              {1, 0, 0, 1, 0, 0}, 1, nullptr, D2D1_LAYER_OPTIONS_NONE};
            target_->PushLayer(parameters, layer.Get());
            target_->DrawBitmap(bitmap.Get(), destination, paint.opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            target_->PopLayer();
            break;
        }
        case Paint::Kind::Icon:
        {
            const float side = std::min(box.w, box.h), unit = side / 24;
            const float left = box.x + (box.w - side) / 2, top = box.y + (box.h - side) / 2;
            ComPtr<ID2D1StrokeStyle> style;
            factory_->CreateStrokeStyle({D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                         D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0},
                                        nullptr, 0, style.GetAddressOf());
            brush->SetColor(Color(paint.color, paint.opacity));
            for (const auto& stroke : lunanx::ui::IconStrokes(paint.text))
                for (std::size_t i = 1; i < stroke.size(); ++i)
                    target_->DrawLine({left + stroke[i - 1].x * unit, top + stroke[i - 1].y * unit},
                                      {left + stroke[i].x * unit, top + stroke[i].y * unit}, brush, 1.85f * unit,
                                      style.Get());
            break;
        }
        case Paint::Kind::Shade:
        {
            // LibraryScreen's shade, top to bottom.
            const std::uint32_t colors[] = {0xbd110f1d, 0x55100d1c, 0x28100c17, 0xd8090612, 0xff090611};
            D2D1_GRADIENT_STOP stops[5];
            for (int i = 0; i < 5; ++i) stops[i] = {i / 4.f, Color(colors[i])};
            ComPtr<ID2D1GradientStopCollection> collection;
            target_->CreateGradientStopCollection(stops, 5, collection.GetAddressOf());
            ComPtr<ID2D1LinearGradientBrush> gradient;
            target_->CreateLinearGradientBrush({{0, rect.top}, {0, rect.bottom}}, collection.Get(), gradient.GetAddressOf());
            target_->FillRectangle(rect, gradient.Get());
            break;
        }
        }
    }

    ComPtr<ID2D1Bitmap> Bitmap(const std::string& address)
    {
        if (auto found = bitmaps_.find(address); found != bitmaps_.end()) return found->second;
        auto decoded = decoded_.find(address);
        if (decoded == decoded_.end() || !decoded->second) return {};
        ComPtr<ID2D1Bitmap> bitmap;
        target_->CreateBitmapFromWicBitmap(decoded->second.Get(), nullptr, bitmap.GetAddressOf());
        bitmaps_[address] = bitmap;
        return bitmap;
    }

    // Artwork from Amazon's image hosts and the application icon, decoded by WIC off the interface thread.
    void LoadImages()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            std::string address;
            {
                std::unique_lock lock(images_mutex_);
                images_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) break;
                address = std::move(queue_.front());
                queue_.pop_front();
            }
            ComPtr<IWICBitmap> picture;
            try { picture = Decode(address); }
            catch (const std::exception&) {}
            Post([this, address, picture] {
                decoded_[address] = picture;
                Redraw();
            });
        }
        CoUninitialize();
    }

    ComPtr<IWICBitmap> Decode(const std::string& address)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        if (address == "lunaone:logo")
        {
            wchar_t path[MAX_PATH] {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring file(path);
            file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"LunaONE.png";
            if (FAILED(wic_->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                       decoder.GetAddressOf())))
                return {};
        }
        else
        {
            const auto host = address.substr(8, address.find('/', 8) - 8);
            if (!address.starts_with("https://") ||
                !(host.ends_with(".media-amazon.com") || host.ends_with(".images-amazon.com")))
                return {};
            const auto response = lunanx::HttpClient {}.Get(address, "LunaONE", {}, 3, 8);
            if (response.status_code != 200 || response.body.empty() || response.body.size() > 8 * 1024 * 1024) return {};
            ComPtr<IWICStream> stream;
            if (FAILED(wic_->CreateStream(stream.GetAddressOf())) ||
                FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(response.body.data())),
                                                    static_cast<DWORD>(response.body.size()))) ||
                FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                     decoder.GetAddressOf())))
                return {};
            // The stream reads the response: decode fully before it goes away.
            return Convert(decoder.Get());
        }
        return Convert(decoder.Get());
    }

    ComPtr<IWICBitmap> Convert(IWICBitmapDecoder* decoder)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(wic_->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) ||
            FAILED(wic_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, bitmap.GetAddressOf())))
            return {};
        return bitmap;
    }

    lunanx::ui::LibraryScene scene_;
    HWND window_ = nullptr;
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_ {sizeof(WINDOWPLACEMENT)};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<IWICImagingFactory> wic_;
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> formats_;
    std::map<std::string, ComPtr<IWICBitmap>> decoded_;
    std::map<std::string, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::set<std::string> requested_;
    std::deque<std::string> queue_;
    std::mutex images_mutex_;
    std::condition_variable images_ready_;
    bool stopping_ = false;
    std::thread images_;
    std::mutex tasks_mutex_;
    std::deque<std::function<void()>> tasks_;
    std::optional<lunaone::desktop::SdlControllers> controllers_;
    std::chrono::steady_clock::time_point next_clock_ {};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const lunaone::desktop::AccountLock lock;
        const lunaone::desktop::CurlRuntime curl;
        Desktop desktop;
        return desktop.Run(instance, show);
    }
    catch (const std::exception& error)
    {
        MessageBoxW(nullptr, Wide(error.what()).c_str(), L"LunaONE", MB_OK | MB_ICONERROR);
        return 1;
    }
}
