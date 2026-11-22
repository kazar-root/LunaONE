// SPDX-License-Identifier: GPL-3.0-only
// LunaONE for Linux: the shared Google TV library (ui/library_scene) drawn
// with Cairo and Pango in a GTK window, driven by the mouse, the keyboard or a
// controller read through SDL. Playback is lunaone::linux_desktop::Play.
#include "player.hpp"
#include "../desktop/runtime.hpp"
#include "core/http_client.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "input/controller_labels.hpp"
#include "ui/library_scene.hpp"

#include <SDL.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace
{
using lunanx::ui::Font;
using lunanx::ui::Paint;
using Pic = std::shared_ptr<GdkPixbuf>;

void Color(cairo_t* cr, std::uint32_t argb, float opacity = 1)
{
    cairo_set_source_rgba(cr, ((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0, (argb & 255) / 255.0,
                          ((argb >> 24) & 255) / 255.0 * opacity);
}

void RoundedPath(cairo_t* cr, double x, double y, double w, double h, double r)
{
    r = std::min({r, w / 2, h / 2});
    if (r <= 0)
    {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

std::filesystem::path ExecutableDirectory()
{
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
}

// "Wi-Fi" or "LAN" for the interface of the default route.
std::string NetworkLabel()
{
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line))
    {
        char name[64] = {};
        unsigned long destination = 1;
        if (std::sscanf(line.c_str(), "%63s %lx", name, &destination) == 2 && destination == 0)
            return std::filesystem::exists(std::filesystem::path("/sys/class/net") / name / "wireless") ? "Wi-Fi" : "LAN";
    }
    return {};
}

int BatteryPercent()
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", error))
        if (entry.path().filename().string().starts_with("BAT"))
        {
            int value = -1;
            std::ifstream(entry.path() / "capacity") >> value;
            if (value >= 0 && value <= 100) return value;
        }
    return -1;
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
        if (controller_) SDL_GameControllerClose(controller_);
    }

    void Run()
    {
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), "LunaONE");
        gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 720);
        gtk_window_set_icon_name(GTK_WINDOW(window_), "lunaone");
        gtk_window_set_icon_from_file(GTK_WINDOW(window_), (ExecutableDirectory() / "LunaONE.png").c_str(), nullptr);
        g_signal_connect(window_, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
        area_ = gtk_drawing_area_new();
        gtk_widget_set_can_focus(area_, TRUE);
        gtk_widget_add_events(area_, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                                         GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
        gtk_container_add(GTK_CONTAINER(window_), area_);
        g_signal_connect(area_, "draw", G_CALLBACK((+[](GtkWidget* widget, cairo_t* cr, gpointer self) -> gboolean {
            static_cast<Desktop*>(self)->Paint(cr, gtk_widget_get_allocated_width(widget),
                                               gtk_widget_get_allocated_height(widget));
            return TRUE;
        })), this);
        g_signal_connect(area_, "button-press-event", G_CALLBACK((+[](GtkWidget*, GdkEventButton* event, gpointer self) -> gboolean {
            if (event->button != 1) return FALSE;
            if (event->type == GDK_BUTTON_PRESS || event->type == GDK_2BUTTON_PRESS)
                static_cast<Desktop*>(self)->scene_.Click(static_cast<float>(event->x), static_cast<float>(event->y),
                                                          event->type == GDK_2BUTTON_PRESS ? 2 : 1);
            return TRUE;
        })), this);
        g_signal_connect(area_, "motion-notify-event", G_CALLBACK((+[](GtkWidget*, GdkEventMotion*, gpointer self) -> gboolean {
            static_cast<Desktop*>(self)->scene_.PointerMoved();
            return FALSE;
        })), this);
        g_signal_connect(area_, "scroll-event", G_CALLBACK((+[](GtkWidget*, GdkEventScroll* event, gpointer self) -> gboolean {
            double dx = 0, dy = 0;
            if (event->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy);
            else if (event->direction == GDK_SCROLL_UP) dy = -1;
            else if (event->direction == GDK_SCROLL_DOWN) dy = 1;
            else if (event->direction == GDK_SCROLL_LEFT) dx = -1;
            else if (event->direction == GDK_SCROLL_RIGHT) dx = 1;
            // Wheel steps of 40 pixels, scrolled content follows the fingers.
            static_cast<Desktop*>(self)->scene_.Wheel(static_cast<float>(-dx * 40), static_cast<float>(-dy * 40));
            return TRUE;
        })), this);
        g_signal_connect(window_, "key-press-event", G_CALLBACK((+[](GtkWidget*, GdkEventKey* event, gpointer self) -> gboolean {
            return static_cast<Desktop*>(self)->Key(event);
        })), this);
        g_signal_connect(window_, "window-state-event", G_CALLBACK((+[](GtkWidget*, GdkEventWindowState* event, gpointer self) -> gboolean {
            static_cast<Desktop*>(self)->fullscreen_ = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
            static_cast<Desktop*>(self)->Redraw();
            return FALSE;
        })), this);
        // Controller, clock and animations: a 60 Hz tick.
        g_timeout_add(16, +[](gpointer self) -> gboolean {
            static_cast<Desktop*>(self)->Tick();
            return G_SOURCE_CONTINUE;
        }, this);
        OpenControllers();
        images_ = std::thread([this] { LoadImages(); });
        gtk_widget_show_all(window_);
        gtk_widget_grab_focus(area_);
        if (lunanx::LoadPlaybackPreferences().fullscreen) gtk_window_fullscreen(GTK_WINDOW(window_));
        ReportDisplay();
        scene_.Start();
        gtk_main();
    }

    // SceneHost
    void Post(std::function<void()> task) override
    {
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
            try { (*static_cast<std::function<void()>*>(data))(); }
            catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
            return G_SOURCE_REMOVE;
        }, new std::function<void()>(std::move(task)), +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    }

    void Redraw() override
    {
        if (area_) gtk_widget_queue_draw(area_);
    }

    float TextWidth(const std::string& text, float size, Font font) override
    {
        auto layout = Layout(text, size, font, -1, false);
        int width = 0, height = 0;
        pango_layout_get_pixel_size(layout.get(), &width, &height);
        return static_cast<float>(width);
    }

    float TextHeight(const std::string& text, float size, Font font, float width) override
    {
        auto layout = Layout(text, size, font, width, true);
        int w = 0, height = 0;
        pango_layout_get_pixel_size(layout.get(), &w, &height);
        return static_cast<float>(height);
    }

    bool ImageReady(const std::string& address) override
    {
        if (images_cache_.contains(address)) return images_cache_[address] != nullptr;
        if (address == "lunaone:logo")
        {
            GdkPixbuf* logo = gdk_pixbuf_new_from_file((ExecutableDirectory() / "LunaONE.png").c_str(), nullptr);
            images_cache_[address] = logo ? Pic(logo, g_object_unref) : nullptr;
            return logo != nullptr;
        }
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
        // The player opens SDL for itself and shuts it down when the game ends.
        if (controller_) SDL_GameControllerClose(controller_);
        controller_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        struct Reopen
        {
            Desktop* self;
            ~Reopen() { self->OpenControllers(); }
        } reopen{this};
        lunaone::linux_desktop::Play(GTK_WINDOW(window_), client, game);
    }

    bool Fullscreen() override { return fullscreen_; }

    void SetFullscreen(bool fullscreen) override
    {
        if (fullscreen) gtk_window_fullscreen(GTK_WINDOW(window_));
        else gtk_window_unfullscreen(GTK_WINDOW(window_));
        fullscreen_ = fullscreen;
    }

    void OpenUrl(const std::string& address) override
    {
        gtk_show_uri_on_window(GTK_WINDOW(window_), address.c_str(), GDK_CURRENT_TIME, nullptr);
    }

    void Quit() override { gtk_widget_destroy(window_); }

    std::string SystemStatus() override
    {
        const std::time_t now = std::time(nullptr);
        char clock[16] = {};
        std::strftime(clock, sizeof(clock), "%H:%M", std::localtime(&now));
        std::string status = clock;
        if (const auto network = NetworkLabel(); !network.empty()) status += " · " + network;
        if (const int battery = BatteryPercent(); battery >= 0) status += " · " + std::to_string(battery) + "%";
        return status;
    }

    // Renders the scene into a PNG without a window, to check the layout.
    void Snapshot(const std::string& path, int seconds)
    {
        auto loop = g_main_loop_new(nullptr, FALSE);
        images_ = std::thread([this] { LoadImages(); });
        scene_.Start();
        // LUNAONE_SNAPSHOT_DEMO=1: a few public covers, without an account.
        if (std::getenv("LUNAONE_SNAPSHOT_DEMO"))
        {
            std::vector<lunanx::luna::Game> games;
            const char* const covers[] = {"B1aAZe7VMLL", "91vEktb7H7L", "B18bK8qu3vL", "A1B8RfQ6+-L", "B1zPBo+nagL",
                                          "C1jur-SHZhL", "91KENrzYk8L", "91xr3LEUJQL", "81wxZcCl3dL", "81RPs7YObdL",
                                          "B1aAZe7VMLL", "91vEktb7H7L"};
            for (int i = 0; i < 12; ++i)
            {
                lunanx::luna::Game game;
                game.title = "Demo " + std::to_string(i + 1);
                game.asin = "D" + std::to_string(i);
                game.image_portrait = std::string("https://m.media-amazon.com/images/I/") + covers[i] + ".png";
                game.image_landscape = "https://m.media-amazon.com/images/I/D1p70L4uifL.png";
                game.publisher = "Publisher";
                game.genres = {"Action", "Aventure"};
                games.push_back(std::move(game));
            }
            scene_.ShowGames(std::make_shared<lunanx::luna::LunaClient>(lunanx::luna::CookieJar {}), std::move(games));
        }
        // LUNAONE_SNAPSHOT_KEYS=up,right,enter,…: keys pressed before the render.
        if (const char* keys = std::getenv("LUNAONE_SNAPSHOT_KEYS"))
        {
            using lunanx::ui::Move;
            const std::map<std::string, Move> names {{"up", Move::Up}, {"down", Move::Down}, {"left", Move::Left},
                {"right", Move::Right}, {"enter", Move::Activate}, {"back", Move::Back}, {"menu", Move::Menu}};
            std::string list = keys;
            for (std::size_t start = 0; start <= list.size();)
            {
                const auto end = std::min(list.find(',', start), list.size());
                if (const auto found = names.find(list.substr(start, end - start)); found != names.end())
                    scene_.Navigate(found->second, lunanx::ui::InputDevice::Keyboard);
                start = end + 1;
            }
        }
        struct Shot
        {
            Desktop* self;
            std::string path;
            GMainLoop* loop;
        } shot{this, path, loop};
        // A frame every half second, as a window would draw, so that the
        // artwork is requested and the backdrop fades in.
        g_timeout_add(500, +[](gpointer data) -> gboolean {
            auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1280, 720);
            auto cr = cairo_create(surface);
            static_cast<Desktop*>(data)->Paint(cr, 1280, 720);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            return G_SOURCE_CONTINUE;
        }, this);
        g_timeout_add_seconds(seconds, +[](gpointer data) -> gboolean {
            auto& shot = *static_cast<Shot*>(data);
            auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1280, 720);
            auto cr = cairo_create(surface);
            shot.self->Paint(cr, 1280, 720);
            cairo_destroy(cr);
            cairo_surface_write_to_png(surface, shot.path.c_str());
            cairo_surface_destroy(surface);
            g_main_loop_quit(shot.loop);
            return G_SOURCE_REMOVE;
        }, &shot);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    }

  private:
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> Layout(const std::string& text, float size, Font font,
                                                                   float width, bool wrap)
    {
        if (!measure_)
        {
            auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            measure_ = cairo_create(surface);
            cairo_surface_destroy(surface);
        }
        std::unique_ptr<PangoLayout, decltype(&g_object_unref)> layout(pango_cairo_create_layout(measure_), g_object_unref);
        Configure(layout.get(), text, size, font, width, wrap);
        return layout;
    }

    static void Configure(PangoLayout* layout, const std::string& text, float size, Font font, float width, bool wrap)
    {
        auto description = pango_font_description_from_string(font == Font::Mono ? "Monospace" : "Sans");
        pango_font_description_set_absolute_size(description, size * PANGO_SCALE);
        pango_font_description_set_weight(description, font == Font::Regular ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD);
        pango_layout_set_font_description(layout, description);
        pango_font_description_free(description);
        pango_layout_set_text(layout, text.c_str(), -1);
        if (width > 0)
        {
            pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
            if (wrap) pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
            else pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
    }

    void Paint(cairo_t* cr, int width, int height)
    {
        const auto paints = scene_.Draw(static_cast<float>(width), static_cast<float>(height));
        for (const auto& paint : paints)
        {
            const auto& box = paint.box;
            switch (paint.kind)
            {
            case Paint::Kind::Rect:
                if ((paint.color >> 24) != 0)
                {
                    RoundedPath(cr, box.x, box.y, box.w, box.h, paint.radius);
                    Color(cr, paint.color, paint.opacity);
                    cairo_fill(cr);
                }
                if (paint.stroke > 0)
                {
                    const double inset = paint.stroke / 2;
                    RoundedPath(cr, box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset,
                                std::max(0.0, paint.radius - inset));
                    Color(cr, paint.stroke_color, paint.opacity);
                    cairo_set_line_width(cr, paint.stroke);
                    cairo_stroke(cr);
                }
                break;
            case Paint::Kind::Text:
            {
                auto layout = pango_cairo_create_layout(cr);
                Configure(layout, paint.text, paint.size, paint.font, paint.box.w, paint.wrap);
                pango_layout_set_alignment(layout, paint.align == lunanx::ui::Align::Center ? PANGO_ALIGN_CENTER :
                                                   paint.align == lunanx::ui::Align::Right  ? PANGO_ALIGN_RIGHT
                                                                                            : PANGO_ALIGN_LEFT);
                int w = 0, h = 0;
                pango_layout_get_pixel_size(layout, &w, &h);
                cairo_save(cr);
                cairo_rectangle(cr, box.x - 2, box.y - 2, box.w + 4, box.h + 4);
                cairo_clip(cr);
                cairo_move_to(cr, box.x, paint.middle ? box.y + (box.h - h) / 2 : box.y);
                Color(cr, paint.color, paint.opacity);
                pango_cairo_show_layout(cr, layout);
                cairo_restore(cr);
                g_object_unref(layout);
                break;
            }
            case Paint::Kind::Image:
            {
                const auto found = images_cache_.find(paint.text);
                if (found == images_cache_.end() || !found->second) break;
                GdkPixbuf* image = found->second.get();
                const double iw = gdk_pixbuf_get_width(image), ih = gdk_pixbuf_get_height(image);
                const double scale = paint.fit ? std::min(box.w / iw, box.h / ih) : std::max(box.w / iw, box.h / ih);
                cairo_save(cr);
                RoundedPath(cr, box.x, box.y, box.w, box.h, paint.radius);
                cairo_clip(cr);
                cairo_translate(cr, box.x + (box.w - iw * scale) / 2, box.y + (box.h - ih * scale) / 2);
                cairo_scale(cr, scale, scale);
                gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                cairo_paint_with_alpha(cr, paint.opacity);
                cairo_restore(cr);
                break;
            }
            case Paint::Kind::Icon:
            {
                const double side = std::min(box.w, box.h), unit = side / 24;
                cairo_save(cr);
                cairo_translate(cr, box.x + (box.w - side) / 2, box.y + (box.h - side) / 2);
                cairo_set_line_width(cr, 1.85 * unit);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                Color(cr, paint.color, paint.opacity);
                for (const auto& stroke : lunanx::ui::IconStrokes(paint.text))
                {
                    for (std::size_t i = 0; i < stroke.size(); ++i)
                        (i ? cairo_line_to : cairo_move_to)(cr, stroke[i].x * unit, stroke[i].y * unit);
                    cairo_stroke(cr);
                }
                cairo_restore(cr);
                break;
            }
            case Paint::Kind::Shade:
            {
                // LibraryScreen's shade, top to bottom.
                auto gradient = cairo_pattern_create_linear(0, box.y, 0, box.y + box.h);
                const std::uint32_t stops[] = {0xbd110f1d, 0x55100d1c, 0x28100c17, 0xd8090612, 0xff090611};
                for (int i = 0; i < 5; ++i)
                    cairo_pattern_add_color_stop_rgba(gradient, i / 4.0, ((stops[i] >> 16) & 255) / 255.0,
                                                      ((stops[i] >> 8) & 255) / 255.0, (stops[i] & 255) / 255.0,
                                                      (stops[i] >> 24) / 255.0);
                cairo_rectangle(cr, box.x, box.y, box.w, box.h);
                cairo_set_source(cr, gradient);
                cairo_fill(cr);
                cairo_pattern_destroy(gradient);
                break;
            }
            }
        }
    }

    gboolean Key(GdkEventKey* event)
    {
        using lunanx::ui::InputDevice;
        using lunanx::ui::Move;
        const auto key = event->keyval;
        const bool control = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK);
        if (key == GDK_KEY_F11)
        {
            scene_.ToggleFullscreen();
            return TRUE;
        }
        if (key == GDK_KEY_Escape)
        {
            // Esc closes a panel, otherwise leaves full screen for the window.
            if (!scene_.Escape() && fullscreen_) scene_.ToggleFullscreen();
            return TRUE;
        }
        if (key == GDK_KEY_BackSpace)
        {
            scene_.Backspace();
            return TRUE;
        }
        const std::pair<guint, Move> moves[] = {{GDK_KEY_Left, Move::Left}, {GDK_KEY_Right, Move::Right},
                                                {GDK_KEY_Up, Move::Up}, {GDK_KEY_Down, Move::Down},
                                                {GDK_KEY_Return, Move::Activate}, {GDK_KEY_KP_Enter, Move::Activate}};
        for (const auto& [code, move] : moves)
            if (key == code)
            {
                scene_.Navigate(move, InputDevice::Keyboard);
                return TRUE;
            }
        if (scene_.Typing() && !control)
        {
            const gunichar character = gdk_keyval_to_unicode(key);
            if (character >= 0x20)
            {
                char text[8] = {};
                g_unichar_to_utf8(character, text);
                scene_.TypeText(text);
                return TRUE;
            }
        }
        if (key == GDK_KEY_space)
        {
            scene_.Navigate(Move::Activate, InputDevice::Keyboard);
            return TRUE;
        }
        return FALSE;
    }

    void OpenControllers()
    {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) std::cerr << "SDL: " << SDL_GetError() << '\n';
    }

    void Tick()
    {
        // The controller drives the catalog while the window is in front.
        SDL_GameControllerUpdate();
        if (controller_ && !SDL_GameControllerGetAttached(controller_))
        {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
        if (!controller_)
            for (int i = 0; i < SDL_NumJoysticks() && !controller_; ++i)
                if (SDL_IsGameController(i)) controller_ = SDL_GameControllerOpen(i);
        if (controller_ && gtk_window_is_active(GTK_WINDOW(window_)))
        {
            using namespace lunanx::input;
            GamepadState state;
            const std::pair<SDL_GameControllerButton, std::uint16_t> buttons[] = {
                {SDL_CONTROLLER_BUTTON_A, kA}, {SDL_CONTROLLER_BUTTON_B, kB}, {SDL_CONTROLLER_BUTTON_X, kX},
                {SDL_CONTROLLER_BUTTON_Y, kY}, {SDL_CONTROLLER_BUTTON_DPAD_UP, kDpadUp},
                {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDpadDown}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDpadLeft},
                {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDpadRight}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLeftShoulder},
                {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRightShoulder}, {SDL_CONTROLLER_BUTTON_START, kStart},
                {SDL_CONTROLLER_BUTTON_BACK, kBack}};
            for (const auto& [button, mask] : buttons)
                if (SDL_GameControllerGetButton(controller_, button)) state.buttons |= mask;
            const auto axis = [this](SDL_GameControllerAxis id) { return SDL_GameControllerGetAxis(controller_, id); };
            state.left_x = axis(SDL_CONTROLLER_AXIS_LEFTX);
            state.left_y = static_cast<std::int16_t>(-std::max<int>(axis(SDL_CONTROLLER_AXIS_LEFTY), -32767));
            state.left_trigger = static_cast<std::uint8_t>(std::max<int>(0, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)) * 255 / 32767);
            state.right_trigger = static_cast<std::uint8_t>(std::max<int>(0, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) * 255 / 32767);
            const auto type = SDL_GameControllerGetType(controller_);
            const auto family = type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5
                ? ControllerFamily::PlayStation
                : type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO ? ControllerFamily::Nintendo : ControllerFamily::Xbox;
            scene_.Gamepad(&state, family);
        }
        else scene_.Gamepad(nullptr, lunanx::input::ControllerFamily::Xbox);
        const auto now = std::chrono::steady_clock::now();
        if (scene_.Animating() || now >= next_clock_)
        {
            if (now >= next_clock_) next_clock_ = now + std::chrono::seconds(30);
            Redraw();
        }
    }

    void ReportDisplay()
    {
        auto display = gdk_display_get_default();
        auto monitor = display ? gdk_display_get_monitor_at_window(display, gtk_widget_get_window(window_)) : nullptr;
        if (!monitor) return;
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        lunanx::SetDeviceDisplay({geometry.height * gdk_monitor_get_scale_factor(monitor),
                                  gdk_monitor_get_refresh_rate(monitor) / 1000});
    }

    // Artwork from Amazon's image hosts, decoded off the interface thread.
    void LoadImages()
    {
        for (;;)
        {
            std::string address;
            {
                std::unique_lock lock(images_mutex_);
                images_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                address = std::move(queue_.front());
                queue_.pop_front();
            }
            Pic picture;
            try
            {
                const auto host = address.substr(8, address.find('/', 8) - 8);
                if (address.starts_with("https://") &&
                    (host.ends_with(".media-amazon.com") || host.ends_with(".images-amazon.com")))
                {
                    const auto response = lunanx::HttpClient{}.Get(address, "LunaONE", {}, 3, 8);
                    if (response.status_code == 200 && response.body.size() < 8 * 1024 * 1024)
                    {
                        std::unique_ptr<GdkPixbufLoader, decltype(&g_object_unref)> loader(gdk_pixbuf_loader_new(), &g_object_unref);
                        if (gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(response.body.data()),
                                                    response.body.size(), nullptr) &&
                            gdk_pixbuf_loader_close(loader.get(), nullptr))
                            if (auto bitmap = gdk_pixbuf_loader_get_pixbuf(loader.get()))
                                picture = Pic(GDK_PIXBUF(g_object_ref(bitmap)), g_object_unref);
                    }
                }
            }
            catch (const std::exception&) {}
            Post([this, address, picture] {
                images_cache_[address] = picture;
                Redraw();
            });
        }
    }

    lunanx::ui::LibraryScene scene_;
    GtkWidget* window_ = nullptr;
    GtkWidget* area_ = nullptr;
    cairo_t* measure_ = nullptr;
    bool fullscreen_ = false;
    SDL_GameController* controller_ = nullptr;
    std::chrono::steady_clock::time_point next_clock_ {};
    std::map<std::string, Pic> images_cache_;
    std::set<std::string> requested_;
    std::deque<std::string> queue_;
    std::mutex images_mutex_;
    std::condition_variable images_ready_;
    bool stopping_ = false;
    std::thread images_;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const lunaone::desktop::AccountLock lock;
        const lunaone::desktop::CurlRuntime curl;
        // Development aid: LUNAONE_SNAPSHOT=<file.png> renders the catalog
        // without a display, after LUNAONE_SNAPSHOT_SECONDS (default 8).
        if (const char* snapshot = std::getenv("LUNAONE_SNAPSHOT"))
        {
            const char* seconds = std::getenv("LUNAONE_SNAPSHOT_SECONDS");
            Desktop{}.Snapshot(snapshot, seconds ? std::max(1, std::atoi(seconds)) : 8);
            return 0;
        }
        if (!gtk_init_check(&argc, &argv)) throw std::runtime_error("Could not open the graphical display.");
        Desktop{}.Run();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "LunaONE: " << error.what() << '\n';
        return 1;
    }
}
