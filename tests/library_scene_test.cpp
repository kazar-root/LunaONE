// SPDX-License-Identifier: GPL-3.0-only
// The shared Windows and Linux library, driven without a window or network.

#include "core/i18n.hpp"
#include "ui/library_scene.hpp"

#include <algorithm>
#include <cassert>
#include <deque>

namespace
{
using namespace lunanx;

class Host final : public ui::SceneHost
{
  public:
    void Post(std::function<void()> task) override { tasks.push_back(std::move(task)); }
    void Redraw() override { ++redraws; }
    float TextWidth(const std::string& text, float size, ui::Font) override { return text.size() * size * .55f; }
    float TextHeight(const std::string&, float size, ui::Font, float) override { return size * 1.3f; }
    bool ImageReady(const std::string&) override { return false; }
    void Play(const luna::LunaClient&, const luna::Game& game) override { played.push_back(game.title); }
    bool Fullscreen() override { return fullscreen; }
    void SetFullscreen(bool value) override { fullscreen = value; }
    void OpenUrl(const std::string&) override {}
    void Quit() override {}
    std::string SystemStatus() override { return "12:00 · LAN"; }

    std::deque<std::function<void()>> tasks;
    std::vector<std::string> played;
    int redraws = 0;
    bool fullscreen = false;
};

bool HasText(const std::vector<ui::Paint>& paints, const std::string& text)
{
    return std::ranges::any_of(paints, [&](const auto& paint) {
        return paint.kind == ui::Paint::Kind::Text && paint.text.find(text) != std::string::npos;
    });
}
} // namespace

int main()
{
    SetUiLanguageForMarket("fr");
    Host host;
    ui::LibraryScene scene(host);

    // Sign-in screen: the TV layout, scaled to the window.
    auto paints = scene.Draw(1280, 720);
    assert(HasText(paints, "LunaONE") && HasText(paints, "Créer un code"));

    std::vector<luna::Game> games;
    for (int i = 0; i < 14; ++i)
    {
        luna::Game game;
        game.title = "Jeu " + std::to_string(i + 1);
        game.asin = "B" + std::to_string(i);
        games.push_back(game);
    }
    scene.ShowGames(std::make_shared<luna::LunaClient>(luna::CookieJar {}), games);
    paints = scene.Draw(1920, 1080);
    assert(HasText(paints, "Jeu 1") && HasText(paints, "Tous les jeux") && HasText(paints, "JOUER"));
    // The first cover starts at the left margin: the carousel is not scrolled.
    const auto first = std::ranges::find_if(paints, [](const auto& paint) { return paint.text == "Jeu 1"; });
    assert(first != paints.end() && first->box.x < 150 && first->box.x > 0);
    assert(HasText(paints, "1 / 2"));

    // Controller: right moves the selection; past the page it turns.
    input::GamepadState pad;
    const auto press = [&](std::uint16_t buttons) {
        pad = {};
        pad.buttons = buttons;
        scene.Gamepad(&pad, input::ControllerFamily::PlayStation);
        pad = {};
        scene.Gamepad(&pad, input::ControllerFamily::PlayStation);
    };
    press(input::kDpadRight);
    assert(scene.Selected() == 1);
    for (int i = 0; i < 9; ++i) press(input::kDpadRight);
    assert(scene.Selected() == 10);
    paints = scene.Draw(1280, 720);
    assert(HasText(paints, "2 / 2") && HasText(paints, "Jeu 11"));
    // Hints name the PlayStation buttons.
    assert(HasText(paints, "L1 + L2 + R1 + R2") && HasText(paints, "L3 + R3 + R1"));

    // L1 + L2 + R1 + R2 opens the search with the keyboard; ✕ types.
    pad = {};
    pad.buttons = input::kLeftShoulder | input::kRightShoulder;
    pad.left_trigger = pad.right_trigger = 255;
    scene.Gamepad(&pad, input::ControllerFamily::PlayStation);
    pad = {};
    scene.Gamepad(&pad, input::ControllerFamily::PlayStation);
    assert(scene.PanelOpen() && scene.KeyboardShown());
    press(input::kA);
    press(input::kA);
    assert(scene.SearchText() == "aa" && scene.PanelOpen());
    // ○ deletes, then hides the keyboard once the field is empty.
    press(input::kB);
    press(input::kB);
    press(input::kB);
    assert(scene.SearchText().empty() && !scene.KeyboardShown() && scene.PanelOpen());
    assert(scene.Escape() && !scene.PanelOpen());
    assert(!scene.Escape());

    // Settings: two panes, one level back at a time.
    for (int i = 0; i < 2; ++i) scene.Navigate(ui::Move::Up, ui::InputDevice::Keyboard);
    for (int i = 0; i < 6; ++i) scene.Navigate(ui::Move::Right, ui::InputDevice::Keyboard);
    scene.Navigate(ui::Move::Activate, ui::InputDevice::Keyboard);
    assert(scene.SettingsOpen());
    paints = scene.Draw(1280, 720);
    assert(HasText(paints, "Général") && HasText(paints, "Manette") && HasText(paints, "Réglages avancés"));
    scene.Navigate(ui::Move::Right, ui::InputDevice::Keyboard);
    scene.Navigate(ui::Move::Activate, ui::InputDevice::Keyboard);
    paints = scene.Draw(1280, 720);
    assert(HasText(paints, "‹   Langue") && HasText(paints, "Automatique"));
    assert(scene.Escape() && scene.SettingsOpen());
    scene.Navigate(ui::Move::Left, ui::InputDevice::Keyboard);
    for (int i = 0; i < 4; ++i) scene.Navigate(ui::Move::Down, ui::InputDevice::Keyboard);
    paints = scene.Draw(1280, 720);
    assert(HasText(paints, "À propos") && HasText(paints, "LunaONE  V0.1") &&
           HasText(paints, "Projet non affilié à Amazon ni à Amazon Luna."));
    assert(scene.Escape() && !scene.SettingsOpen());
    // Back down from the icons to the covers.
    scene.Navigate(ui::Move::Down, ui::InputDevice::Keyboard);

    // Keyboard and mouse: Enter plays, a click on the selected cover plays.
    scene.Navigate(ui::Move::Activate, ui::InputDevice::Keyboard);
    assert(host.played.size() == 1 && host.played.back() == "Jeu 11");
    scene.ToggleFullscreen();
    assert(host.fullscreen);
    paints = scene.Draw(1280, 720);
    assert(HasText(paints, "Esc") && HasText(paints, "Fenêtre"));
    scene.ToggleFullscreen();
}
