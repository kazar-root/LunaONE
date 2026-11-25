// SPDX-License-Identifier: GPL-3.0-only
// The Google TV library (app/android/.../LibraryScreen.java) for the Windows
// and Linux clients: layout, focus, panels, on-screen keyboard and the Luna
// jobs behind them, written once. A platform draws the paints it returns and
// forwards the mouse, keyboard and controller. The Mac client draws the same
// screen with AppKit (app/apple/macos_catalog.mm).
#pragma once

#include "input/controller_labels.hpp"
#include "input/luna_input_encoder.hpp"
#include "luna/luna_client.hpp"
#include "ui/virtual_keyboard.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lunanx::ui
{
enum class Font { Regular, Bold, Mono };
enum class Align { Left, Center, Right };

struct Box
{
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Point
{
    float x, y;
};

// One drawing operation, in window pixels, painted in order.
struct Paint
{
    // Shade: LibraryScreen's vertical gradient over the backdrop. Backdrop: the
    // sign-in screen's violet, deepening to night blue (#3a1f9a, #1b1150 at
    // 55 %, #0c0820) from the top left, with two soft glows: #7c4dff at 44 %
    // around (82 %, 18 %), radius 45 % of the width, and #b06bff at 25 %
    // around (8 %, 92 %), radius 40 %.
    enum class Kind { Rect, Text, Image, Icon, Shade, Backdrop } kind = Kind::Rect;
    Box box;
    // Rect fill, text colour or icon stroke, as 0xAARRGGBB.
    std::uint32_t color = 0;
    // Rect and image corner radius.
    float radius = 0;
    float stroke = 0;
    std::uint32_t stroke_color = 0;
    // Text, image address ("lunaone:logo" for the application icon) or icon name.
    std::string text;
    float size = 0;
    Font font = Font::Regular;
    Align align = Align::Left;
    // Text: wrap within the width; otherwise one line ending with an ellipsis.
    bool wrap = false;
    // Text: centred vertically in the box.
    bool middle = true;
    float opacity = 1;
    // Image: whole picture inside the box instead of filling it.
    bool fit = false;
};

// LibraryScreen.Icon in a 24 × 24 grid, as polylines stroked 1.85 wide.
std::vector<std::vector<Point>> IconStrokes(const std::string& name);

enum class Move { Left, Right, Up, Down, Activate, Back, Menu, Search, Space, PreviousSection, NextSection };
enum class InputDevice { Mouse, Keyboard, Gamepad };

// What the scene needs from the platform. Called on the interface thread,
// except Post, which any thread may call.
class SceneHost
{
  public:
    virtual ~SceneHost() = default;
    virtual void Post(std::function<void()> task) = 0;
    virtual void Redraw() = 0;
    virtual float TextWidth(const std::string& text, float size, Font font) = 0;
    virtual float TextHeight(const std::string& text, float size, Font font, float width) = 0;
    // Starts loading the picture if needed and redraws once it has arrived.
    virtual bool ImageReady(const std::string& address) = 0;
    // Opens the game and returns once it has ended; throws on failure.
    virtual void Play(const luna::LunaClient& client, const luna::Game& game) = 0;
    virtual bool Fullscreen() = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;
    virtual void OpenUrl(const std::string& address) = 0;
    virtual void Quit() = 0;
    // Clock, connection and battery for the header.
    virtual std::string SystemStatus() = 0;
};

class LibraryScene
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit LibraryScene(SceneHost& host);
    ~LibraryScene();
    LibraryScene(const LibraryScene&) = delete;
    LibraryScene& operator=(const LibraryScene&) = delete;

    // Loads the saved account, if any.
    void Start();
    // Shows games without signing in, for tests and layout checks.
    void ShowGames(std::shared_ptr<luna::LunaClient> client, std::vector<luna::Game> games);
    // State seen by tests.
    bool PanelOpen() const { return panel_; }
    bool SettingsOpen() const { return settings_; }
    bool KeyboardShown() const { return keyboard_ != nullptr; }
    std::size_t Selected() const { return selected_; }
    const std::string& SearchText() const { return field_text_; }
    std::vector<Paint> Draw(float width, float height);
    // True while something moves: the next frame should be drawn soon.
    bool Animating() const;

    void Navigate(Move move, InputDevice device);
    void Click(float x, float y, int clicks);
    void PointerMoved();
    void Wheel(float dx, float dy);
    // The search field takes typed text.
    bool Typing() const;
    void TypeText(const std::string& text);
    void Backspace();
    // Esc: closes a panel and returns true; false when there is none, and the
    // platform then leaves full screen.
    bool Escape();
    void ToggleFullscreen();
    // The controller state, read about 60 times a second, or none.
    void Gamepad(const input::GamepadState* state, input::ControllerFamily family);

  private:
    struct Row
    {
        // 0 action, 1 heading, 2 information, 3 paragraph, 4 button at the bottom.
        int kind = 0;
        std::string title, value;
        bool checked = false;
        std::function<void()> action;
    };
    struct Hit
    {
        Box box;
        std::function<void(int)> action;
    };
    // A row of the settings screen: 0 choice with a list of values, 1 switch,
    // 2 action, 3 note.
    struct SettingsEntry
    {
        std::string title, value;
        int kind = 0;
        int choice = -1;
        bool on = false;
        std::function<void()> action;
    };
    struct SettingsCategory
    {
        std::string title, icon;
        std::vector<SettingsEntry> rows;
    };
    enum Zone { kTop, kCovers, kActions };
    enum Action { kPlay, kPrevious, kNext, kCancel };

    // Layout.
    Box At(float x, float y, float w, float h) const;
    void Rect(Box box, std::uint32_t color, float radius = 0, float stroke = 0, std::uint32_t stroke_color = 0,
              float opacity = 1);
    void Label(Box box, const std::string& text, float size, std::uint32_t color, Font font = Font::Regular,
               Align align = Align::Left, bool wrap = false, bool middle = true);
    void Image(Box box, const std::string& address, float radius = 0, float opacity = 1, bool fit = false);
    void Icon(Box box, const std::string& name, std::uint32_t color);
    void Clickable(Box box, std::function<void(int)> action);
    float Width(const std::string& text, float size, Font font);

    void DrawLoading();
    void DrawLogin();
    void DrawLibrary();
    void DrawNavigation(const std::string& icon, const std::string& text, bool selected, float x, float y, float w,
                        int index, std::function<void()> action, const std::string& avatar);
    void DrawCovers();
    void DrawDetails();
    void DrawPanel();
    void DrawKeyboard(float left, float top, float width);
    void Pill(Box box, const std::string& title, bool primary, bool focused, std::function<void()> action);
    void LoginButton(float y, const std::string& title, bool primary, std::function<void()> action, float x = 88,
                     float w = 480);
    std::string Hints() const;
    std::string Name(input::PadButton button) const;

    // State changes.
    void Select(std::size_t index);
    void TurnPage(int direction);
    void OpenSection(int section);
    void CoverClicked(std::size_t index, int clicks);
    void LibraryMove(Move move);
    void LoginMove(Move move);
    void PanelMove(Move move);
    void KeyboardMove(Move move);
    void ToggleKeyboard();
    void SetTrack(float offset, bool animated);
    float TrackOffset() const;
    void FollowSelection(bool immediate);
    void Used(InputDevice device);

    // Panels.
    void OpenPanel(std::string title, std::vector<Row> rows, std::function<void()> back);
    void ClosePanel();
    void ShowAccount();
    void ShowSearch();
    void SubmitSearch();
    void ShowSettings(int focus_choice);
    void CloseSettings();
    std::vector<SettingsCategory> SettingsCategories();
    void DrawSettings();
    void SettingsMove(Move move);
    void SettingsActivate(const SettingsEntry& entry, int row);
    void ApplyChoice(std::size_t index, std::size_t value);
    void ShowMarkets();
    void ConfirmSignOut();
    void CheckUpdates();

    // Luna jobs on the worker thread.
    void Run(std::function<void()> job);
    void Connect();
    void Search(std::string query);
    void Explore();
    void ExploreQuietly();
    void AddExplored(std::shared_ptr<luna::LunaClient> client, std::vector<luna::Game> games);
    void SignOut();
    void PlaySelected();
    void Cancel();

    SceneHost& host_;
    std::vector<Paint> paints_;
    std::vector<Hit> hits_;
    float scale_ = 1, width_ = 1280, offset_y_ = 0, pixel_width_ = 1280, pixel_height_ = 720;
    // The sign-in screen centres its 1280-wide design; the library uses the full width.
    float offset_x_ = 0;

    std::shared_ptr<luna::LunaClient> client_;
    std::vector<luna::Shelf> shelves_;
    std::vector<luna::Game> catalog_, shown_;
    luna::PlayerProfile profile_;
    std::string query_, market_ = "fr", message_, code_, code_address_;
    bool saved_ = false, busy_ = false, loading_ = false, simple_ = false;
    int section_ = 2;
    std::size_t selected_ = 0, first_ = 0;
    std::thread worker_;
    std::atomic<bool> cancelled_ {false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    // Quiet exploration of the whole catalog after loading.
    std::shared_ptr<std::atomic<bool>> exploring_;

    Zone zone_ = kCovers;
    int top_index_ = 3, action_index_ = kPlay;
    std::size_t login_focus_ = 0, login_count_ = 0;
    InputDevice device_ = InputDevice::Mouse;

    // Animations.
    float track_from_ = 0, track_to_ = 0, track_length_ = 0;
    Clock::time_point track_started_ {};
    std::size_t grown_ = 0, shrinking_ = SIZE_MAX;
    Clock::time_point grow_started_ {};
    std::string backdrop_shown_, backdrop_previous_, backdrop_wanted_;
    Clock::time_point backdrop_due_ {}, fade_started_ {}, loading_since_ {};

    // Panel.
    bool panel_ = false;
    std::string panel_title_;
    std::vector<Row> rows_;
    int panel_focus_ = -1, panel_first_ = 0;
    std::function<void()> panel_back_;
    bool field_ = false, field_focused_ = false;
    std::string field_text_;
    std::unique_ptr<VirtualKeyboard> keyboard_;
    bool checking_updates_ = false;

    // Settings screen: column 0 categories, 1 rows, 2 values of a row.
    bool settings_ = false;
    int settings_category_ = 0, settings_column_ = 0, settings_row_ = 0, settings_value_ = 0, settings_editing_ = -1,
        settings_first_ = 0;
    // Pane entrance: 1 deeper, 2 back, 3 another category.
    int settings_motion_ = 0;
    Clock::time_point settings_motion_at_ {};

    // Controller.
    input::ControllerFamily family_ = input::ControllerFamily::Xbox;
    std::uint32_t pad_ = 0, shoulders_ = 0;
    bool latched_ = false, chord_held_ = false;
    int repeat_move_ = 0;
    Clock::time_point repeat_at_ {};
};
} // namespace lunanx::ui
