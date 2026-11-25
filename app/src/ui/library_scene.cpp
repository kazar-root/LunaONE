// SPDX-License-Identifier: GPL-3.0-only

#include "library_scene.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "core/update_check.hpp"
#include "luna/amazon_auth.hpp"
#include "stream/launch_status.hpp"
#include "ui/device_qr.hpp"
#include "ui/image_url_policy.hpp"
#include "ui/library_sections.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <set>

namespace lunanx::ui
{
namespace
{
using namespace std::chrono_literals;
using luna::AccountPlan;
using luna::AmazonAuth;
using luna::DeviceRegistration;
using luna::Game;

// LibraryScreen.java colours.
constexpr std::uint32_t kBackground = 0xff100d1d, kText = 0xfffaf8ff, kSecondary = 0xffc7bdd4, kViolet = 0xffaa88ff,
                        kGreen = 0xff78e3bd, kInk = 0xff151125, kShell = 0xe818142b, kCard = 0xff2a2240,
                        kRank = 0xff6f42ea, kQuiet = 0x8dd8cce8, kRing = 0x867d718e, kPill = 0xfff0eaff;
// The focus fill of the settings rows.
constexpr std::uint32_t kFocus = 0xff5640bb;
// The sign-in screen in the library's night colours, as on the Mac: frosted
// cards on the logo's violet, violet buttons with a white ring on focus.
constexpr std::uint32_t kLoginCard = 0x1fffffff, kLoginCardEdge = 0x40aa88ff, kLoginButton = 0xff6a4cff,
                        kLoginButtonFocus = 0xff8466ff, kLoginSecondary = 0x1affffff,
                        kLoginSecondaryFocus = 0x55aa88ff, kLoginOutline = 0x4dffffff;
constexpr std::size_t kPageSize = 10;
const char* const kSections[] = {"Top 10", "Reprendre", "Tous les jeux", "Luna Premium"};

float Progress(LibraryScene::Clock::time_point start, std::chrono::milliseconds length)
{
    const auto elapsed = LibraryScene::Clock::now() - start;
    return std::clamp(std::chrono::duration<float>(elapsed).count() / std::chrono::duration<float>(length).count(), 0.f, 1.f);
}

std::vector<Point> Arc(float l, float t, float r, float b, float start, float sweep, bool close = false)
{
    std::vector<Point> points;
    const float cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
    const int steps = std::max(8, static_cast<int>(std::abs(sweep) / 10));
    for (int i = 0; i <= steps; ++i)
    {
        const float angle = (start + sweep * i / steps) * std::numbers::pi_v<float> / 180;
        points.push_back({cx + rx * std::cos(angle), cy + ry * std::sin(angle)});
    }
    if (close) points.push_back(points.front());
    return points;
}

std::vector<Point> RoundedSquare(float l, float t, float r, float b)
{
    std::vector<Point> points;
    const auto corner = [&](float cx, float cy, float start) {
        const auto arc = Arc(cx - 2, cy - 2, cx + 2, cy + 2, start, 90);
        points.insert(points.end(), arc.begin(), arc.end());
    };
    corner(r - 2, t + 2, 270);
    corner(r - 2, b - 2, 0);
    corner(l + 2, b - 2, 90);
    corner(l + 2, t + 2, 180);
    points.push_back(points.front());
    return points;
}
} // namespace

std::vector<std::vector<Point>> IconStrokes(const std::string& name)
{
    std::vector<std::vector<Point>> strokes;
    const auto circle = [&](float x, float y, float r) { strokes.push_back(Arc(x - r, y - r, x + r, y + r, 0, 360, true)); };
    if (name == "clock")
    {
        circle(12, 12, 8);
        strokes.push_back({{12, 7}, {12, 12}, {15, 13}});
    }
    else if (name == "search")
    {
        circle(10, 10, 5.8f);
        strokes.push_back({{14.5f, 14.5f}, {20, 20}});
    }
    else if (name == "grid")
    {
        for (float x : {4.f, 13.f})
            for (float y : {4.f, 13.f}) strokes.push_back(RoundedSquare(x, y, x + 7, y + 7));
    }
    else if (name == "trophy")
    {
        strokes.push_back(RoundedSquare(8, 4, 16, 14));
        strokes.push_back({{8, 6}, {4, 6}, {5, 10}, {8, 11}});
        strokes.push_back({{16, 6}, {20, 6}, {19, 10}, {16, 11}});
        strokes.push_back({{12, 14}, {12, 20}});
        strokes.push_back({{8, 20}, {16, 20}});
    }
    else if (name == "account")
    {
        circle(12, 8, 3.5f);
        strokes.push_back(Arc(5, 13, 19, 25, 186, 168));
    }
    else if (name == "settings")
    {
        circle(12, 12, 6);
        circle(12, 12, 2.1f);
        for (int i = 0; i < 8; ++i)
        {
            const float angle = i * std::numbers::pi_v<float> / 4;
            strokes.push_back({{12 + 9 * std::sin(angle), 12 - 9 * std::cos(angle)},
                               {12 + 6 * std::sin(angle), 12 - 6 * std::cos(angle)}});
        }
    }
    else if (name == "lock")
    {
        strokes.push_back(RoundedSquare(6, 10, 18, 20));
        strokes.push_back(Arc(8, 4, 16, 14, 180, 180));
    }
    else if (name == "gamepad")
    {
        // Body with rounded ends, a cross and two buttons.
        auto body = Arc(3, 7, 13, 18, 90, 180);
        const auto right = Arc(11, 7, 21, 18, 270, 180);
        body.insert(body.end(), right.begin(), right.end());
        body.push_back(body.front());
        strokes.push_back(body);
        strokes.push_back({{7, 12.5f}, {10, 12.5f}});
        strokes.push_back({{8.5f, 11}, {8.5f, 14}});
        circle(15.5f, 11.5f, .7f);
        circle(17.5f, 13.5f, .7f);
    }
    else if (name == "display")
    {
        strokes.push_back({{5, 5}, {19, 5}, {21, 7}, {21, 14}, {19, 16}, {5, 16}, {3, 14}, {3, 7}, {5, 5}});
        strokes.push_back({{12, 16}, {12, 19}});
        strokes.push_back({{8, 19.5f}, {16, 19.5f}});
    }
    else if (name == "info")
    {
        circle(12, 12, 8.5f);
        strokes.push_back({{12, 11}, {12, 16.5f}});
        circle(12, 7.8f, .5f);
    }
    return strokes;
}

LibraryScene::LibraryScene(SceneHost& host) : host_(host) {}

LibraryScene::~LibraryScene()
{
    if (exploring_) exploring_->store(false);
    alive_->store(false);
    cancelled_ = true;
    if (worker_.joinable()) worker_.join();
}

void LibraryScene::Start()
{
    DeviceRegistration registration;
    saved_ = AmazonAuth::Load(registration);
    const auto preferences = LoadPlaybackPreferences();
    simple_ = preferences.simple_library;
    if (!preferences.market.empty()) market_ = preferences.market;
    SetUiLanguageForMarket(saved_ ? registration.market_code : preferences.market);
    if (saved_) Connect();
}


void LibraryScene::ShowGames(std::shared_ptr<luna::LunaClient> client, std::vector<Game> games)
{
    client_ = std::move(client);
    catalog_ = games;
    shown_ = std::move(games);
    section_ = 2;
    query_.clear();
    selected_ = 0;
    loading_ = busy_ = false;
    FollowSelection(true);
    host_.Redraw();
}

Box LibraryScene::At(float x, float y, float w, float h) const
{
    return {offset_x_ + x * scale_, offset_y_ + y * scale_, std::max(0.f, w * scale_), std::max(0.f, h * scale_)};
}

void LibraryScene::Rect(Box box, std::uint32_t color, float radius, float stroke, std::uint32_t stroke_color, float opacity)
{
    Paint paint;
    paint.box = box;
    paint.color = color;
    paint.radius = radius * scale_;
    paint.stroke = stroke * scale_;
    paint.stroke_color = stroke_color;
    paint.opacity = opacity;
    paints_.push_back(std::move(paint));
}

void LibraryScene::Label(Box box, const std::string& text, float size, std::uint32_t color, Font font, Align align,
                         bool wrap, bool middle)
{
    if (text.empty()) return;
    Paint paint;
    paint.kind = Paint::Kind::Text;
    paint.box = box;
    paint.text = text;
    paint.color = color;
    paint.size = size * scale_;
    paint.font = font;
    paint.align = align;
    paint.wrap = wrap;
    paint.middle = middle;
    paints_.push_back(std::move(paint));
}

void LibraryScene::Image(Box box, const std::string& address, float radius, float opacity, bool fit)
{
    if (address.empty() || !host_.ImageReady(address)) return;
    Paint paint;
    paint.kind = Paint::Kind::Image;
    paint.box = box;
    paint.text = address;
    paint.radius = radius * scale_;
    paint.opacity = opacity;
    paint.fit = fit;
    paints_.push_back(std::move(paint));
}

void LibraryScene::Icon(Box box, const std::string& name, std::uint32_t color)
{
    Paint paint;
    paint.kind = Paint::Kind::Icon;
    paint.box = box;
    paint.text = name;
    paint.color = color;
    paints_.push_back(std::move(paint));
}

void LibraryScene::Clickable(Box box, std::function<void(int)> action) { hits_.push_back({box, std::move(action)}); }

float LibraryScene::Width(const std::string& text, float size, Font font)
{
    return host_.TextWidth(text, size * scale_, font) / scale_;
}



std::vector<Paint> LibraryScene::Draw(float width, float height)
{
    paints_.clear();
    hits_.clear();
    pixel_width_ = width;
    pixel_height_ = height;
    scale_ = std::max(0.1f, std::min(width / 1280, height / 720));
    width_ = width / scale_;
    offset_y_ = (height - 720 * scale_) / 2;
    offset_x_ = 0;
    login_count_ = 0;
    const bool signing_in = !client_ && !loading_;
    if (signing_in)
    {
        Paint backdrop;
        backdrop.kind = Paint::Kind::Backdrop;
        backdrop.box = {0, 0, width, height};
        paints_.push_back(backdrop);
    }
    else
    {
        Rect({0, 0, width, height}, kBackground);
        // The selected game's artwork, cross-fading, under the shade.
        const auto now = Clock::now();
        if (!backdrop_wanted_.empty() && backdrop_wanted_ != backdrop_shown_ && now >= backdrop_due_ &&
            host_.ImageReady(backdrop_wanted_))
        {
            backdrop_previous_ = backdrop_shown_;
            backdrop_shown_ = backdrop_wanted_;
            fade_started_ = now;
        }
        const float fade = Progress(fade_started_, 350ms);
        if (fade >= 1) backdrop_previous_.clear();
        if (client_)
        {
            Image({0, 0, width, height}, backdrop_previous_);
            Image({0, 0, width, height}, backdrop_shown_, 0, fade);
        }
        Paint shade;
        shade.kind = Paint::Kind::Shade;
        shade.box = {0, 0, width, height};
        paints_.push_back(shade);
    }
    if (loading_) DrawLoading();
    else if (client_) DrawLibrary();
    else DrawLogin();
    if (settings_) DrawSettings();
    if (panel_) DrawPanel();
    return std::move(paints_);
}

bool LibraryScene::Animating() const
{
    const auto now = Clock::now();
    return loading_ || now - track_started_ < 260ms || now - grow_started_ < 170ms || now - fade_started_ < 360ms ||
           now - settings_motion_at_ < 240ms ||
           (backdrop_wanted_ != backdrop_shown_ && now < backdrop_due_ + 50ms);
}

void LibraryScene::DrawLoading()
{
    Image(At(width_ / 2 - 48, 208, 96, 96), "lunaone:logo", 22);
    Label(At(0, 322, width_, 48), "LunaONE", 34, kText, Font::Bold, Align::Center);
    Label(At(80, 378, width_ - 160, 32), message_.empty() ? Tr("Chargement…") : message_, 19, kSecondary,
          Font::Regular, Align::Center);
    const int active = LaunchPulseIndex(Clock::now() - loading_since_);
    for (int i = 0; i < 3; ++i)
        Rect(At(width_ / 2 - 26.5f + i * 21, 428, 11, 11), 0xffffffff, 2, 0, 0, i == active ? 1 : .23f);
    Pill(At(width_ / 2 - 67, 476, 134, 52), Tr("Annuler"), false, device_ != InputDevice::Mouse, [this] { Cancel(); });
}

void LibraryScene::LoginButton(float y, const std::string& title, bool primary, std::function<void()> action, float x,
                               float w)
{
    const std::size_t index = login_count_++;
    const bool focused = device_ != InputDevice::Mouse && index == login_focus_;
    const auto box = At(x, y, w, 56);
    Rect(box, primary ? (focused ? kLoginButtonFocus : kLoginButton) : (focused ? kLoginSecondaryFocus : kLoginSecondary),
         12, focused ? 3 : (primary ? 0 : 1), focused ? (primary ? 0xffffffff : kViolet) : kLoginOutline);
    Label(box, title, 19, kText, Font::Bold, Align::Center);
    Clickable(box, [this, index, action](int) {
        login_focus_ = index;
        action();
    });
}

// The form on the left and, on the right, the steps of the sign-in or the
// code with its QR, as on the Mac. The 1280 × 720 design scales to fit the
// window in both directions and stays centred, so that it never leaves a TV's
// picture.
void LibraryScene::DrawLogin()
{
    offset_x_ = std::max(0.f, (pixel_width_ - 1280 * scale_) / 2);
    Image(At(88, 56, 72, 72), "lunaone:logo", 16);
    Label(At(176, 56, 440, 46), "LunaONE", 40, kText, Font::Bold);
    Label(At(178, 102, 440, 26), Tr("Connexion Amazon"), 18, kViolet);
    Label(At(88, 148, 480, 48), Tr("Tes jeux Amazon Luna, sans navigateur, sur tous tes écrans."), 17, kSecondary,
          Font::Regular, Align::Left, true, false);

    // The form, on a frosted card.
    constexpr float kCardTop = 212, kFormX = 112, kFormWidth = 432;
    const float card_height = saved_ ? 22 + 3 * 56 + 2 * 12 + 22 : 22 + 34 + 3 * 56 + 2 * 12 + 22;
    Rect(At(88, kCardTop, 480, card_height), kLoginCard, 18, 1, kLoginCardEdge);
    float y = kCardTop + 22;
    if (saved_)
    {
        LoginButton(y, Tr("Réessayer le chargement"), true, [this] { Connect(); }, kFormX, kFormWidth);
        LoginButton(y += 68, Tr("Se déconnecter"), false, [this] { ConfirmSignOut(); }, kFormX, kFormWidth);
    }
    else
    {
        Label(At(kFormX, y, kFormWidth, 26), Tr("Pays du compte"), 16, kSecondary);
        y += 34;
        std::string market = market_;
        for (const auto& entry : luna::AmazonMarkets())
            if (entry.code == market_) market = entry.native_name;
        LoginButton(y, market + "   ▾", false, [this] { ShowMarkets(); }, kFormX, kFormWidth);
        LoginButton(y += 68, Tr("Créer un code"), true, [this] { Connect(); }, kFormX, kFormWidth);
    }
    LoginButton(y += 68, Tr("Réglages"), false, [this] { ShowSettings(-1); }, kFormX, kFormWidth);

    // The right card.
    Rect(At(640, 112, 552, 456), kLoginCard, 18, 1, kLoginCardEdge);
    if (!code_.empty())
    {
        Label(At(668, 136, 496, 50), Tr("Scanne le QR code et saisis le code Amazon."), 18, kText, Font::Bold,
              Align::Center, true, false);
        // The QR keeps a white plate: phone cameras read it best that way.
        Rect(At(640 + (552 - 212) / 2.f, 196, 212, 212), 0xffffffff, 16);
        const auto qr = MakeDeviceQr(code_address_);
        if (qr.size)
        {
            const float side = 180, cell = side / qr.size, left = 640 + (552 - side) / 2, top = 212;
            for (int row = 0; row < qr.size; ++row)
                for (int column = 0; column < qr.size; ++column)
                    if (qr.dark[row * qr.size + column])
                        Rect(At(left + column * cell, top + row * cell, cell + .05f, cell + .05f), 0xff000000);
        }
        Label(At(668, 426, 496, 30), code_address_, 19, kViolet, Font::Regular, Align::Center);
        Label(At(668, 462, 496, 72), code_, 50, kText, Font::Mono, Align::Center);
    }
    else
    {
        Label(At(672, 140, 488, 32), Tr("Comment se connecter"), 22, kText, Font::Bold);
        const char* steps[] = {"Choisis le pays de ton compte Amazon, puis crée un code.",
                               "Scanne le QR code avec ton téléphone ou ouvre la page Amazon.",
                               "Saisis le code et confirme : ta bibliothèque Luna s’ouvre ici."};
        for (int i = 0; i < 3; ++i)
        {
            const float top = 200 + i * 104;
            Rect(At(672, top, 44, 44), kLoginButton, 22);
            Label(At(672, top, 44, 44), std::to_string(i + 1), 20, kText, Font::Bold, Align::Center);
            Label(At(736, top, 424, 76), Tr(steps[i]), 17, kSecondary, Font::Regular, Align::Left, true, false);
        }
        Label(At(672, 516, 488, 30), Tr("Aucun mot de passe n’est saisi dans LunaONE."), 15, kViolet);
    }

    y = kCardTop + card_height;
    Label(At(88, y + 12, 480, 28), message_, 16, kSecondary);
    if (busy_) LoginButton(y + 48, Tr("Annuler"), false, [this] { Cancel(); }, 88, 480);
    if (login_focus_ >= login_count_) login_focus_ = login_count_ ? login_count_ - 1 : 0;
    Label(At(88, 682, 900, 30), Hints(), 13, kSecondary);
    // The settings and panels drawn next use the full width.
    offset_x_ = 0;
}

void LibraryScene::DrawNavigation(const std::string& icon, const std::string& text, bool selected, float x, float y,
                                  float w, int index, std::function<void()> action, const std::string& avatar)
{
    const float diameter = 47;
    const bool focused = device_ != InputDevice::Mouse && zone_ == kTop && top_index_ == index;
    const bool marked = selected || focused;
    const auto disc = At(x + (w - diameter) / 2, y, diameter, diameter);
    Rect(disc, focused ? 0xff3a2c66 : kShell, diameter / 2, marked ? 2 : 1, marked ? kViolet : kRing);
    Icon(At(x + (w - diameter) / 2 + diameter / 4, y + diameter / 4, diameter / 2, diameter / 2), icon, kText);
    if (!avatar.empty()) Image(disc, avatar, diameter / 2);
    Label(At(x, y + diameter + 3, w, 21), text, 14, marked ? kViolet : kText, Font::Bold, Align::Center);
    Clickable(At(x, y, w, diameter + 25), [this, index, action](int) {
        if (busy_) return;
        zone_ = kTop;
        top_index_ = index;
        action();
    });
}

void LibraryScene::DrawLibrary()
{
    // Header: logo, name, clock and connection, account and settings.
    Rect(At(40, 21, 46, 46), kShell, 12);
    Image(At(40, 21, 46, 46), "lunaone:logo", 12);
    Label(At(96, 21, 143, 46), "LunaONE", 25, kText, Font::Bold);
    Rect(At(width_ - 638, 20, 372, 48), kShell, 24, 1, 0x668e80a6);
    std::string summary = host_.SystemStatus();
    const auto plan = client_->Plan();
    const auto plan_label = plan == AccountPlan::Premium ? std::string("Luna Premium") :
                            plan == AccountPlan::Standard ? std::string("Luna Standard") : client_->PlanLabel();
    if (!plan_label.empty()) summary += "   │   " + plan_label;
    Label(At(width_ - 630, 20, 356, 48), summary, 15, kText, Font::Regular, Align::Center);

    // Section icons, then account and settings: the top focus row (1 to 6).
    // « Tous les jeux » holds the whole catalog, Luna Premium included.
    const bool searching = !query_.empty();
    const char* const icons[] = {"trophy", "clock", "grid"};
    for (int i = 0; i < 3; ++i)
        DrawNavigation(icons[i], Tr(kSections[i]), (section_ == i || (i == 2 && section_ == 3)) && !searching,
                       35 + i * 111, 78, 111, i + 1, [this, i] { OpenSection(i); }, {});
    DrawNavigation("search", Tr("Rechercher"), searching, 35 + 3 * 111, 78, 111, 4, [this] { ShowSearch(); }, {});
    DrawNavigation("account", Tr("Compte"), false, width_ - 241, 7, 86, 5, [this] { ShowAccount(); }, profile_.avatar_url);
    DrawNavigation("settings", Tr("Réglages"), false, width_ - 133, 7, 86, 6, [this] { ShowSettings(-1); }, {});

    // Heading: violet accent, section and count, status on the right.
    Rect(At(64, 165, 4, 20), kViolet, 2);
    const auto heading = searching ? Tr("Rechercher") : Tr(kSections[section_]);
    Label(At(80, 157, 510, 36), heading, 20, kText, Font::Bold);
    Label(At(80 + Width(heading, 20, Font::Bold) + 14, 157, 300, 36),
          TrFormat("{} jeux", {std::to_string(shown_.size())}), 15, kSecondary);
    Label(At(width_ / 2, 157, width_ / 2 - 64, 36), message_, 14, kSecondary, Font::Regular, Align::Right);

    DrawCovers();
    DrawDetails();
    Label(At(64, 682, std::max(200.f, width_ - 260), 30), Hints(), 13, kSecondary);
    Label(At(width_ - 165, 682, 130, 30), VersionLabel(), 13, kText, Font::Regular, Align::Right);
}

void LibraryScene::DrawCovers()
{
    const float start = 64, y = 211;
    first_ = selected_ / kPageSize * kPageSize;
    const std::size_t last = std::min(first_ + kPageSize, shown_.size());
    const bool ranked = section_ == 0 && query_.empty();
    const auto plan = client_->Plan();
    const float grow = Progress(grow_started_, 160ms);
    const float offset = TrackOffset();
    if (shown_.empty())
    {
        Label(At(start, y + 47, width_ - start * 2, 56), Tr("Aucun jeu dans cette rubrique."), 24, kSecondary);
        return;
    }
    if (simple_)
    {
        const float width = std::min(width_ - 2 * start - 8, 590.f);
        track_length_ = (last - first_) * 47 + 8;
        for (std::size_t i = first_; i < last; ++i)
        {
            const float top = 4 + (i - first_) * 47 - offset;
            if (top < -42 || top > 280) continue;
            const auto box = At(start + 4, y + top, width, 42);
            const bool chosen = i == selected_;
            Rect(box, kCard, 17, chosen ? 3 : 1, chosen ? kViolet : kQuiet);
            Label(At(start + 16, y + top, width - 24, 42),
                  (ranked ? "#" + std::to_string(i + 1) + "  " : std::string()) + shown_[i].title, 18, kText, Font::Bold);
            Clickable(box, [this, i](int clicks) { CoverClicked(i, clicks); });
        }
        return;
    }
    const float card = 192, height = 256, gap = 22;
    track_length_ = start * 2 + (last - first_) * (card + gap);
    for (std::size_t i = first_; i < last; ++i)
    {
        const auto& game = shown_[i];
        const auto presentation = PresentLibraryGame(game, plan);
        // The selected cover grows by 5.5 % around its centre, as on the TV.
        const float factor = i == selected_ ? grow : i == shrinking_ ? 1 - grow : 0;
        const float extra = .0275f * factor;
        const float x = start + (i - first_) * (card + gap) - offset;
        if (x + card < -20 || x > width_ + 20) continue;
        const float w = card * (1 + 2 * extra), h = height * (1 + 2 * extra);
        const auto box = At(x - card * extra, y + 12 - height * extra, w, h);
        const bool chosen = i == selected_;
        Rect(box, kCard, 17);
        Label(At(x + 8 - card * extra, y + 12 - height * extra, w - 16, h), game.title, 18, kText, Font::Bold,
              Align::Center, true, true);
        Image(box, ResizeAmazonImageUrl(presentation.cover, 480), 17);
        Rect(box, 0, 17, chosen ? 3 : 1, chosen ? kViolet : kQuiet);
        if (ranked)
        {
            const auto badge = At(x - card * extra + 11, y + 12 - height * extra + 11, 37, 26);
            Rect(badge, kRank, 13);
            Label(badge, "#" + std::to_string(i + 1), 14, 0xffffffff, Font::Bold, Align::Center);
        }
        if (presentation.locked)
        {
            const auto holder = At(x + card * (1 + extra) - 35, y + 12 - height * extra + 9, 26, 26);
            Rect(holder, kShell, 12);
            Icon(holder, "lock", kText);
        }
        Clickable(box, [this, i](int clicks) { CoverClicked(i, clicks); });
    }
}

void LibraryScene::DrawDetails()
{
    const float left = 64, available = width_ - left * 2;
    const bool keys = device_ != InputDevice::Mouse;
    if (!shown_.empty())
    {
        const auto& game = shown_.at(selected_);
        const auto presentation = PresentLibraryGame(game, client_->Plan());
        Label(At(left, 495, available, 54), game.title, 34, kText, Font::Bold);
        float x = left;
        if (!presentation.offer.empty())
        {
            Label(At(x, 546, available, 32), presentation.offer, 15, presentation.included ? kGreen : kViolet, Font::Bold);
            x += Width(presentation.offer, 15, Font::Bold) + 14;
        }
        Label(At(x, 546, left + available - x, 32), presentation.metadata, 15, kSecondary);
        Pill(At(left, 594, 220, 52), "▶   " + Tr("JOUER"), true, keys && zone_ == kActions && action_index_ == kPlay,
             [this] {
                 zone_ = kActions;
                 action_index_ = kPlay;
                 PlaySelected();
             });
    }
    if (shown_.size() > kPageSize)
    {
        const std::size_t pages = (shown_.size() + kPageSize - 1) / kPageSize, page = first_ / kPageSize;
        const float x = left + 244;
        if (page > 0)
            Pill(At(x, 598, 42, 44), "‹", false, keys && zone_ == kActions && action_index_ == kPrevious, [this] {
                zone_ = kActions;
                action_index_ = kPrevious;
                TurnPage(-1);
            });
        Label(At(x + 46, 598, 62, 44), std::to_string(page + 1) + " / " + std::to_string(pages), 15, kSecondary,
              Font::Bold, Align::Center);
        if (page + 1 < pages)
            Pill(At(x + 112, 598, 42, 44), "›", false, keys && zone_ == kActions && action_index_ == kNext, [this] {
                zone_ = kActions;
                action_index_ = kNext;
                TurnPage(1);
            });
    }
    if (busy_)
        Pill(At(width_ - left - 134, 594, 134, 52), Tr("Annuler"), false, keys, [this] { Cancel(); });
}

void LibraryScene::Pill(Box box, const std::string& title, bool primary, bool focused, std::function<void()> action)
{
    Rect(box, primary ? kPill : kShell, 26, focused ? 3 : 0, kViolet);
    Label(box, title, primary ? 20 : 17, primary ? kInk : kText, Font::Bold, Align::Center);
    Clickable(box, [action](int) { action(); });
}

std::string LibraryScene::Name(input::PadButton button) const { return input::PadButtonName(family_, button); }

std::string LibraryScene::Hints() const
{
    using input::PadButton;
    const auto preferences = LoadPlaybackPreferences();
    const std::string fullscreen = host_.Fullscreen() ? "Esc  " + Tr("Fenêtre") : "F11  " + Tr("Plein écran");
    switch (device_)
    {
    case InputDevice::Gamepad:
        return Name(preferences.swap_ab ? PadButton::B : PadButton::A) + "  " + Tr("Valider") + "    " +
               Name(preferences.swap_ab ? PadButton::A : PadButton::B) + "  " + Tr("Retour") + "    " +
               Name(PadButton::Menu) + "  " + Tr("Menu") +
               (preferences.virtual_keyboard && client_ ? "    " + input::KeyboardChordName(family_) + "  " + Tr("Clavier") : "") +
               "    " + TrFormat("Quitter une partie : {}", {input::ExitChordName(family_)});
    case InputDevice::Keyboard:
        return "← → ↑ ↓ " + Tr("Naviguer") + "    ↵ " + Tr("Valider") + "    ⌫ " + Tr("Retour") + "    " + fullscreen;
    case InputDevice::Mouse:
        return client_ ? Tr("Double-clic : jouer") + "    " + fullscreen : fullscreen;
    }
    return {};
}

void LibraryScene::DrawPanel()
{
    Rect({0, 0, pixel_width_, pixel_height_}, 0x94000000);
    Clickable({0, 0, pixel_width_, pixel_height_}, [this](int) {
        if (panel_back_) panel_back_();
    });
    const float width = keyboard_ ? 900 : 640, inner = width - 56, limit = keyboard_ ? 680 : 600;
    const float keyboard_height = keyboard_ ? keyboard_->Rows().size() * 56 + 44 : 0;
    const float field_height = field_ ? 62 + keyboard_height : 0;
    std::vector<int> buttons;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].kind == 4) buttons.push_back(static_cast<int>(i));
    const float bar = buttons.empty() ? 0 : 78;
    const auto height_of = [&](const Row& row) -> float {
        switch (row.kind)
        {
        case 1: return 40;
        case 2: return 34;
        case 3: return host_.TextHeight(row.title, 16 * scale_, Font::Regular, inner * scale_) / scale_ + 14;
        case 4: return 0;
        default: return 48;
        }
    };
    const auto fits = [&](int first, int last) {
        float total = 0;
        for (int i = first; i <= last; ++i) total += height_of(rows_[i]);
        return total <= limit - 84 - field_height - bar;
    };
    const int list_focus = panel_focus_ >= 0 && rows_[panel_focus_].kind == 4 ? -1 : panel_focus_;
    if (list_focus >= 0 && list_focus < panel_first_) panel_first_ = list_focus;
    while (list_focus >= 0 && !fits(panel_first_, list_focus)) ++panel_first_;
    if (panel_first_ > 0 && rows_[panel_first_ - 1].kind == 1 && list_focus >= 0 && fits(panel_first_ - 1, list_focus))
        --panel_first_;
    int last = panel_first_;
    float height = 0;
    while (last < static_cast<int>(rows_.size()) && fits(panel_first_, last)) height += height_of(rows_[last++]);
    const float card_height = 84 + field_height + height + bar + 18;
    const float left = (width_ - width) / 2, top = std::max(12.f, (720 - card_height) / 2);
    const auto card = At(left, top, width, card_height);
    Rect(card, 0xff1a1530, 22, 1, 0x668e80a6);
    Clickable(card, [](int) {});
    Label(At(left + 28, top + 22, inner, 40), panel_title_, 24, kText, Font::Bold);
    if (panel_first_ > 0) Label(At(left + width - 44, top + 30, 20, 20), "▲", 12, kSecondary);
    if (last < static_cast<int>(rows_.size())) Label(At(left + width - 44, top + card_height - 30, 20, 20), "▼", 12, kSecondary);
    const bool keys = device_ != InputDevice::Mouse;
    float y = top + 76;
    if (field_)
    {
        // The search field: text centred with a margin, caret at the end.
        Rect(At(left + 28, y, inner, 50), kCard, 12, 2, kViolet);
        const bool empty = field_text_.empty();
        const bool caret = field_focused_ && (std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::now().time_since_epoch()).count() / 530) % 2 == 0;
        Label(At(left + 44, y, inner - 32, 50), empty ? Tr("Titre du jeu") : field_text_ + (caret ? "|" : ""), 19,
              empty ? kSecondary : kText);
        Clickable(At(left + 28, y, inner, 50), [this](int) { field_focused_ = true; });
        if (keyboard_) DrawKeyboard(left, y + 62, width);
        y += field_height;
    }
    for (int i = panel_first_; i < last; ++i)
    {
        const auto& row = rows_[i];
        const float row_height = height_of(row);
        if (row.kind == 4) continue;
        if (row.kind == 1)
            Label(At(left + 30, y + 10, inner, 30), row.title, 14, kViolet, Font::Bold);
        else if (row.kind == 2)
            Label(At(left + 30, y, inner, row_height), row.title, 15, kSecondary);
        else if (row.kind == 3)
            Label(At(left + 30, y + 4, inner, row_height), row.title, 16, kText, Font::Regular, Align::Left, true, false);
        else
        {
            const auto box = At(left + 20, y + 2, width - 40, row_height - 4);
            const bool focused = i == panel_focus_ && (keys || !field_);
            Rect(box, focused ? 0x40aa88ff : 0, 12, focused && keys ? 2 : 0, kViolet);
            Label(At(left + 32, y + 2, inner * .62f, row_height - 4), (row.checked ? "✓  " : "") + row.title, 17,
                  row.checked ? kViolet : kText, row.checked ? Font::Bold : Font::Regular);
            if (!row.value.empty())
                Label(At(left + 20 + inner * .62f, y + 2, width - 52 - inner * .62f, row_height - 4), row.value + "   ›",
                      15, kSecondary, Font::Regular, Align::Right);
            Clickable(box, [this, i](int) {
                panel_focus_ = i;
                if (rows_[i].action) rows_[i].action();
            });
        }
        y += row_height;
    }
    // Buttons along the bottom: the first on the left, the main one on the far right.
    float right = left + width - 28;
    for (std::size_t n = 0; n < buttons.size(); ++n)
    {
        const int i = buttons[buttons.size() - 1 - n];
        const bool primary = n == 0;
        const float button_width = std::max(150.f, Width(rows_[i].title, 17, Font::Bold) + 48);
        const bool leftmost = n + 1 == buttons.size() && buttons.size() > 1;
        const float x = leftmost ? left + 28 : right - button_width;
        right = x - 14;
        const auto box = At(x, top + card_height - 22 - 52, button_width, 52);
        const bool focused = i == panel_focus_ && keys && !field_focused_;
        Rect(box, primary ? kPill : kShell, 26, focused ? 3 : (primary ? 0 : 1), focused ? kViolet : kRing);
        Label(box, rows_[i].title, 17, primary ? kInk : kText, Font::Bold, Align::Center);
        Clickable(box, [this, i](int) {
            panel_focus_ = i;
            if (rows_[i].action) rows_[i].action();
        });
    }
}

void LibraryScene::DrawKeyboard(float left, float top, float width)
{
    const auto& rows = keyboard_->Rows();
    const float unit = 64, gap = 6, height = 50;
    float y = top;
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        int units = 0;
        for (const auto& key : rows[r]) units += key.span;
        float x = left + (width - (units * unit - gap)) / 2;
        for (std::size_t c = 0; c < rows[r].size(); ++c)
        {
            const auto& key = rows[r][c];
            const auto box = At(x, y, key.span * unit - gap, height);
            const bool focused = r == keyboard_->Row() && c == keyboard_->Column();
            const bool active = key.kind == KeyKind::Shift && keyboard_->Shifted();
            Rect(box, focused ? kFocus : active ? 0xff3a2c66 : kCard, 10, focused ? 2 : 0, kViolet);
            Label(box, key.label, key.span > 1 ? 16 : 20, kText, Font::Bold, Align::Center);
            Clickable(box, [this, r, c](int) {
                if (!keyboard_) return;
                keyboard_->Focus(r, c);
                KeyboardMove(Move::Activate);
            });
            x += key.span * unit;
        }
        y += height + gap;
    }
    using input::PadButton;
    const bool swapped = LoadPlaybackPreferences().swap_ab;
    const std::string hint = Name(swapped ? PadButton::B : PadButton::A) + "  " + Tr("Saisir") + "    " +
        Name(swapped ? PadButton::A : PadButton::B) + "  " + Tr("Effacer") + "    " + Name(PadButton::X) + "  " +
        Tr("Espace") + "    " + Name(PadButton::Y) + "  " + Tr("Maj") + "    " + Name(PadButton::Menu) + "  " +
        Tr("Valider") + "    " + input::KeyboardChordName(family_) + "  " + Tr("Masquer le clavier");
    Label(At(left + 20, y + 4, width - 40, 30), hint, 13, kSecondary, Font::Regular, Align::Center);
}



float LibraryScene::TrackOffset() const
{
    const float t = Progress(track_started_, 250ms);
    const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
    return track_from_ + (track_to_ - track_from_) * eased;
}

void LibraryScene::SetTrack(float offset, bool animated)
{
    const float visible = simple_ ? 280 : width_;
    offset = std::clamp(offset, 0.f, std::max(0.f, track_length_ - visible));
    track_from_ = animated ? TrackOffset() : offset;
    track_to_ = offset;
    track_started_ = animated ? Clock::now() : Clock::time_point{};
}

void LibraryScene::FollowSelection(bool immediate)
{
    if (shown_.empty()) return;
    first_ = selected_ / kPageSize * kPageSize;
    const std::size_t last = std::min(first_ + kPageSize, shown_.size());
    // As a float: an unsigned index would wrap below zero.
    const float index = static_cast<float>(selected_ - first_);
    if (simple_)
    {
        track_length_ = (last - first_) * 47 + 8;
        const float top = 4 + index * 47;
        float offset = track_to_;
        if (top < offset) offset = top - 4;
        if (top + 46 > offset + 280) offset = top + 46 - 280;
        SetTrack(offset, !immediate);
    }
    else
    {
        track_length_ = 128 + (last - first_) * 214;
        SetTrack(64 + index * 214 - 104, !immediate);
    }
    const auto presentation = PresentLibraryGame(shown_.at(selected_), client_->Plan());
    backdrop_wanted_ = ResizeAmazonImageUrl(presentation.backdrop, 1280);
    backdrop_due_ = Clock::now() + (immediate ? 0ms : 130ms);
}

void LibraryScene::Select(std::size_t index)
{
    if (index >= shown_.size() || busy_) return;
    const bool same_page = index / kPageSize == selected_ / kPageSize;
    shrinking_ = same_page ? selected_ : SIZE_MAX;
    selected_ = index;
    grow_started_ = same_page ? Clock::now() : Clock::time_point{};
    FollowSelection(!same_page);
    host_.Redraw();
}

void LibraryScene::TurnPage(int direction)
{
    const auto next = direction > 0 ? first_ + kPageSize : first_ == 0 ? shown_.size() : first_ - 1;
    if (next >= shown_.size() || busy_) return;
    selected_ = next;
    shrinking_ = SIZE_MAX;
    grow_started_ = {};
    FollowSelection(true);
    host_.Redraw();
}

void LibraryScene::OpenSection(int section)
{
    if (busy_ || !client_) return;
    const auto sections = std::array{LibrarySection::Top, LibrarySection::Recent, LibrarySection::All, LibrarySection::Premium};
    section_ = section;
    query_.clear();
    shown_ = LibraryGames(sections.at(section), shelves_, catalog_, client_->Plan());
    selected_ = 0;
    shrinking_ = SIZE_MAX;
    grow_started_ = {};
    track_from_ = track_to_ = 0;
    FollowSelection(true);
    host_.Redraw();
}

void LibraryScene::CoverClicked(std::size_t index, int clicks)
{
    if (busy_) return;
    zone_ = kCovers;
    // As on a touch screen: the first click selects, the next one plays.
    if (index == selected_ || clicks >= 2)
    {
        Select(index);
        PlaySelected();
        return;
    }
    Select(index);
}

void LibraryScene::Used(InputDevice device)
{
    if (device_ == device) return;
    device_ = device;
    host_.Redraw();
}



void LibraryScene::Navigate(Move move, InputDevice device)
{
    Used(device);
    if (panel_)
    {
        if (keyboard_ && field_) KeyboardMove(move);
        else PanelMove(move);
    }
    else if (settings_) SettingsMove(move);
    else if (loading_)
    {
        if (move == Move::Activate || move == Move::Back) Cancel();
    }
    else if (!client_) LoginMove(move);
    else LibraryMove(move);
    host_.Redraw();
}

void LibraryScene::LibraryMove(Move move)
{
    if (busy_)
    {
        if (move == Move::Activate || move == Move::Back) Cancel();
        return;
    }
    const bool searching = !query_.empty();
    const int active_top = searching ? 4 : std::min(section_, 2) + 1;
    switch (move)
    {
    case Move::Menu: ShowSettings(-1); return;
    case Move::Search: ShowSearch(); return;
    case Move::PreviousSection: OpenSection((std::min(section_, 2) + 2) % 3); return;
    case Move::NextSection: OpenSection((std::min(section_, 2) + 1) % 3); return;
    case Move::Back:
        if (searching) OpenSection(section_);
        return;
    default: break;
    }
    if (zone_ == kTop)
    {
        if (move == Move::Left) top_index_ = std::max(1, top_index_ - 1);
        else if (move == Move::Right) top_index_ = std::min(6, top_index_ + 1);
        else if (move == Move::Down && !shown_.empty()) zone_ = kCovers;
        else if (move == Move::Activate)
        {
            switch (top_index_)
            {
            case 1: case 2: case 3: OpenSection(top_index_ - 1); break;
            case 4: ShowSearch(); break;
            case 5: ShowAccount(); break;
            default: ShowSettings(-1); break;
            }
        }
    }
    else if (zone_ == kCovers)
    {
        const auto back = simple_ ? Move::Up : Move::Left, forward = simple_ ? Move::Down : Move::Right;
        const std::size_t last = std::min(first_ + kPageSize, shown_.size()) - 1;
        if (move == back)
        {
            if (selected_ > first_) Select(selected_ - 1);
            else if (first_ > 0) TurnPage(-1);
            else if (simple_) { zone_ = kTop; top_index_ = active_top; }
        }
        else if (move == forward)
        {
            if (selected_ < last) Select(selected_ + 1);
            else if (last + 1 < shown_.size()) TurnPage(1);
            else if (simple_) { zone_ = kActions; action_index_ = kPlay; }
        }
        else if (move == Move::Up) { zone_ = kTop; top_index_ = active_top; }
        else if (move == Move::Down) { zone_ = kActions; action_index_ = kPlay; }
        else if (move == Move::Activate) PlaySelected();
    }
    else
    {
        std::vector<int> order{kPlay};
        const std::size_t pages = (shown_.size() + kPageSize - 1) / kPageSize, page = first_ / kPageSize;
        if (shown_.size() > kPageSize && page > 0) order.push_back(kPrevious);
        if (shown_.size() > kPageSize && page + 1 < pages) order.push_back(kNext);
        auto at = std::ranges::find(order, action_index_);
        if (at == order.end()) at = order.begin();
        if (move == Move::Left && at != order.begin()) action_index_ = *(at - 1);
        else if (move == Move::Right && at + 1 != order.end()) action_index_ = *(at + 1);
        else if (move == Move::Up && !shown_.empty()) zone_ = kCovers;
        else if (move == Move::Activate)
        {
            if (*at == kPlay) PlaySelected();
            else TurnPage(*at == kNext ? 1 : -1);
        }
    }
}

void LibraryScene::LoginMove(Move move)
{
    if (login_count_ == 0) return;
    if ((move == Move::Up || move == Move::Left) && login_focus_ > 0) --login_focus_;
    else if ((move == Move::Down || move == Move::Right) && login_focus_ + 1 < login_count_) ++login_focus_;
    else if (move == Move::Activate)
    {
        // The sign-in buttons are the only hits of this screen, in focus order.
        if (login_focus_ < hits_.size()) hits_[login_focus_].action(1);
    }
    else if (move == Move::Back && busy_) Cancel();
    else if (move == Move::Menu) ShowSettings(-1);
}

void LibraryScene::PanelMove(Move move)
{
    const int count = static_cast<int>(rows_.size());
    const bool on_button = panel_focus_ >= 0 && rows_[panel_focus_].kind == 4;
    const auto next = [&](int from, int direction) {
        for (int i = from + direction; i >= 0 && i < count; i += direction)
            if (rows_[i].kind == 0) return i;
        return -1;
    };
    const auto button = [&](int direction) {
        for (int i = panel_focus_ + direction; i >= 0 && i < count; i += direction)
            if (rows_[i].kind == 4) { panel_focus_ = i; return; }
    };
    switch (move)
    {
    case Move::Up:
        if (!on_button) { if (const int row = next(panel_focus_, -1); row >= 0) panel_focus_ = row; }
        else if (const int row = next(count, -1); row >= 0) panel_focus_ = row;
        else if (field_) field_focused_ = true;
        break;
    case Move::Down:
        if (field_focused_) { field_focused_ = false; panel_focus_ = count - 1; break; }
        if (on_button) break;
        if (const int row = next(panel_focus_, 1); row >= 0) panel_focus_ = row;
        else if (!rows_.empty() && rows_.back().kind == 4) panel_focus_ = count - 1;
        break;
    case Move::Left: if (on_button && !field_focused_) button(-1); break;
    case Move::Right: if (on_button && !field_focused_) button(1); break;
    case Move::Activate:
        if (field_ && field_focused_) SubmitSearch();
        else if (panel_focus_ >= 0 && rows_[panel_focus_].action) rows_[panel_focus_].action();
        break;
    case Move::Back:
    case Move::Menu:
        if (panel_back_) panel_back_();
        break;
    default: break;
    }
}

void LibraryScene::KeyboardMove(Move move)
{
    switch (move)
    {
    case Move::Left: keyboard_->Move(-1, 0); break;
    case Move::Right: keyboard_->Move(1, 0); break;
    case Move::Up: keyboard_->Move(0, -1); break;
    case Move::Down: keyboard_->Move(0, 1); break;
    case Move::Search: keyboard_->ToggleShift(); break;
    case Move::Space: field_text_ += ' '; break;
    case Move::Activate:
        if (keyboard_->Press(field_text_)) SubmitSearch();
        break;
    case Move::Back:
        // Delete, or hide the keyboard once the field is empty.
        if (field_text_.empty()) keyboard_.reset();
        else VirtualKeyboard::Backspace(field_text_);
        break;
    case Move::Menu: SubmitSearch(); break;
    default: break;
    }
}

void LibraryScene::ToggleKeyboard()
{
    if (!client_ || busy_ || loading_) return;
    if (!LoadPlaybackPreferences().virtual_keyboard && !keyboard_) return;
    if (!field_)
    {
        ShowSearch();
        if (!field_) return;
        keyboard_ = std::make_unique<VirtualKeyboard>(UiLanguage());
    }
    else if (keyboard_) keyboard_.reset();
    else keyboard_ = std::make_unique<VirtualKeyboard>(UiLanguage());
    host_.Redraw();
}

void LibraryScene::Click(float x, float y, int clicks)
{
    Used(InputDevice::Mouse);
    for (auto hit = hits_.rbegin(); hit != hits_.rend(); ++hit)
        if (hit->box.Contains(x, y))
        {
            const auto action = hit->action;
            action(clicks);
            break;
        }
    host_.Redraw();
}

void LibraryScene::PointerMoved() { Used(InputDevice::Mouse); }

void LibraryScene::Wheel(float dx, float dy)
{
    if (settings_ && !panel_)
    {
        if (std::abs(dy) >= 1 && settings_editing_ < 0)
        {
            settings_first_ = std::max(0, settings_first_ + (dy > 0 ? -1 : 1));
            settings_column_ = std::min(settings_column_, 1);
        }
    }
    else if (panel_)
    {
        if (std::abs(dy) >= 1) PanelMove(dy > 0 ? Move::Up : Move::Down);
    }
    else if (client_ && !loading_)
    {
        const float delta = std::abs(dx) > std::abs(dy) ? dx : dy;
        SetTrack(TrackOffset() - delta / scale_, false);
    }
    host_.Redraw();
}

bool LibraryScene::Typing() const { return panel_ && field_; }

void LibraryScene::TypeText(const std::string& text)
{
    if (!Typing()) return;
    field_focused_ = true;
    field_text_ += text;
    host_.Redraw();
}

void LibraryScene::Backspace()
{
    if (Typing() && !field_text_.empty())
    {
        VirtualKeyboard::Backspace(field_text_);
        host_.Redraw();
    }
    else Navigate(Move::Back, InputDevice::Keyboard);
}

bool LibraryScene::Escape()
{
    Used(InputDevice::Keyboard);
    if (panel_)
    {
        if (panel_back_) panel_back_();
    }
    else if (settings_) SettingsMove(Move::Back);
    else return false;
    host_.Redraw();
    return true;
}

void LibraryScene::ToggleFullscreen()
{
    const bool fullscreen = !host_.Fullscreen();
    host_.SetFullscreen(fullscreen);
    auto preferences = LoadPlaybackPreferences();
    preferences.fullscreen = fullscreen;
    SavePlaybackPreferences(preferences);
    host_.Redraw();
}

void LibraryScene::Gamepad(const input::GamepadState* state, input::ControllerFamily family)
{
    if (!state)
    {
        pad_ = shoulders_ = 0;
        chord_held_ = false;
        return;
    }
    family_ = family;
    const auto& pad = *state;
    const bool swapped = LoadPlaybackPreferences().swap_ab;
    enum : std::uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kMenu = 64, kY = 128, kX = 256 };
    std::uint32_t buttons = 0;
    const auto set = [&](bool on, std::uint32_t bit) { if (on) buttons |= bit; };
    set((pad.buttons & input::kDpadUp) || pad.left_y > 18000, kUp);
    set((pad.buttons & input::kDpadDown) || pad.left_y < -18000, kDown);
    set((pad.buttons & input::kDpadLeft) || pad.left_x < -18000, kLeft);
    set((pad.buttons & input::kDpadRight) || pad.left_x > 18000, kRight);
    set(pad.buttons & (swapped ? input::kB : input::kA), kA);
    set(pad.buttons & (swapped ? input::kA : input::kB), kB);
    set(pad.buttons & input::kStart, kMenu);
    set(pad.buttons & input::kY, kY);
    set(pad.buttons & input::kX, kX);
    // L1 and R1 alone change the section when released; the four shoulders
    // and triggers together show or hide the keyboard.
    const std::uint32_t shoulders = (pad.buttons & input::kLeftShoulder ? 1u : 0u) |
                                    (pad.buttons & input::kRightShoulder ? 2u : 0u) |
                                    (pad.left_trigger >= 128 ? 4u : 0u) | (pad.right_trigger >= 128 ? 8u : 0u);
    if (latched_)
    {
        latched_ = buttons != 0 || shoulders != 0;
        pad_ = buttons;
        shoulders_ = 0;
        return;
    }
    if (VirtualKeyboardChord(pad) && !chord_held_)
    {
        chord_held_ = true;
        Used(InputDevice::Gamepad);
        ToggleKeyboard();
    }
    if (shoulders) shoulders_ |= shoulders;
    else if (shoulders_)
    {
        const auto gesture = shoulders_;
        const bool chord = chord_held_;
        shoulders_ = 0;
        chord_held_ = false;
        if (!chord && (gesture == 1 || gesture == 2))
            Navigate(gesture == 1 ? Move::PreviousSection : Move::NextSection, InputDevice::Gamepad);
    }
    const std::uint32_t pressed = buttons & ~pad_;
    pad_ = buttons;
    if (chord_held_) return;
    const auto now = Clock::now();
    const std::pair<std::uint32_t, Move> directions[] = {{kUp, Move::Up}, {kDown, Move::Down}, {kLeft, Move::Left},
                                                         {kRight, Move::Right}};
    for (const auto& [bit, move] : directions)
    {
        if (pressed & bit)
        {
            repeat_move_ = static_cast<int>(bit);
            repeat_at_ = now + 420ms;
            Navigate(move, InputDevice::Gamepad);
        }
        else if ((buttons & bit) && repeat_move_ == static_cast<int>(bit) && now >= repeat_at_)
        {
            repeat_at_ = now + 110ms;
            Navigate(move, InputDevice::Gamepad);
        }
    }
    const std::pair<std::uint32_t, Move> actions[] = {{kA, Move::Activate}, {kB, Move::Back}, {kMenu, Move::Menu},
                                                      {kY, Move::Search}, {kX, Move::Space}};
    for (const auto& [bit, move] : actions)
        if (pressed & bit)
        {
            Navigate(move, InputDevice::Gamepad);
            return;
        }
}



void LibraryScene::OpenPanel(std::string title, std::vector<Row> rows, std::function<void()> back)
{
    if (title != Tr("Rechercher un jeu"))
    {
        field_ = false;
        keyboard_.reset();
    }
    panel_ = true;
    panel_title_ = std::move(title);
    rows_ = std::move(rows);
    panel_back_ = std::move(back);
    panel_first_ = 0;
    panel_focus_ = -1;
    for (std::size_t i = 0; i < rows_.size() && panel_focus_ < 0; ++i)
        if (rows_[i].kind == 0) panel_focus_ = static_cast<int>(i);
    if (panel_focus_ < 0 && !rows_.empty() && rows_.back().kind == 4) panel_focus_ = static_cast<int>(rows_.size()) - 1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].checked) panel_focus_ = static_cast<int>(i);
    host_.Redraw();
}

void LibraryScene::ClosePanel()
{
    panel_ = false;
    field_ = false;
    field_focused_ = false;
    keyboard_.reset();
    rows_.clear();
    panel_back_ = nullptr;
    host_.Redraw();
}

void LibraryScene::ShowAccount()
{
    if (busy_ || !client_) return;
    std::vector<Row> rows;
    if (!profile_.name.empty()) rows.push_back({2, profile_.name});
    if (const auto plan = client_->PlanLabel(); !plan.empty()) rows.push_back({2, plan});
    rows.push_back({4, Tr("Annuler"), {}, false, [this] { ClosePanel(); }});
    rows.push_back({4, Tr("Se déconnecter"), {}, false, [this] { ConfirmSignOut(); }});
    OpenPanel(Tr("Compte"), std::move(rows), [this] { ClosePanel(); });
}

void LibraryScene::ShowSearch()
{
    if (busy_ || !client_) return;
    const bool keyboard = LoadPlaybackPreferences().virtual_keyboard;
    field_ = true;
    field_focused_ = true;
    field_text_ = query_;
    std::vector<Row> rows{{4, Tr("Annuler"), {}, false, [this] { ClosePanel(); }}};
    if (keyboard)
        rows.push_back({4, Tr("Clavier"), {}, false, [this] {
            if (keyboard_) keyboard_.reset();
            else keyboard_ = std::make_unique<VirtualKeyboard>(UiLanguage());
            host_.Redraw();
        }});
    rows.push_back({4, Tr("Rechercher"), {}, false, [this] { SubmitSearch(); }});
    OpenPanel(Tr("Rechercher un jeu"), std::move(rows), [this] { ClosePanel(); });
    // With a controller in hand, the search opens with the keyboard.
    if (keyboard && device_ == InputDevice::Gamepad) keyboard_ = std::make_unique<VirtualKeyboard>(UiLanguage());
}

void LibraryScene::SubmitSearch()
{
    auto query = field_text_;
    query.erase(0, query.find_first_not_of(" \t"));
    query.erase(query.find_last_not_of(" \t") + 1);
    if (query.empty())
    {
        field_focused_ = true;
        return;
    }
    ClosePanel();
    Search(std::move(query));
}

// The settings screen, as on the Mac: categories on the left, the rows of the
// chosen one on the right. A no/yes or off/on choice is a switch; another
// opens its values in the right pane. Left or Back returns one level.
std::vector<LibraryScene::SettingsCategory> LibraryScene::SettingsCategories()
{
    const auto choices = PlaybackChoices(LoadPlaybackPreferences(), false, false, true);
    std::vector<SettingsCategory> categories;
    const auto section = [&](PlaybackSection which, const char* icon) {
        SettingsCategory category {Tr(PlaybackSectionTitle(which)), icon, {}};
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            const auto& choice = choices[i];
            if (choice.section != which) continue;
            SettingsEntry entry;
            entry.title = choice.title;
            entry.value = choice.labels.at(choice.selected);
            entry.choice = static_cast<int>(i);
            const bool binary = choice.labels.size() == 2 &&
                ((choice.labels[0] == Tr("Non") && choice.labels[1] == Tr("Oui")) ||
                 (choice.labels[0] == Tr("Désactivé") && choice.labels[1] == Tr("Activé")));
            entry.kind = binary ? 1 : 0;
            entry.on = choice.selected == 1;
            category.rows.push_back(std::move(entry));
        }
        return category;
    };
    categories.push_back(section(PlaybackSection::General, "settings"));
    auto controller = section(PlaybackSection::Controller, "gamepad");
    // The shortcuts, named for the controller in hand.
    for (auto note : {TrFormat("Quitter une partie : {}", {input::ExitChordName(family_)}),
                      TrFormat("Clavier virtuel : {}", {input::KeyboardChordName(family_)}),
                      TrFormat("Infos techniques en jeu : {}", {input::OverlayChordName(family_)})})
        controller.rows.push_back({std::move(note), {}, 3});
    categories.push_back(std::move(controller));
    categories.push_back(section(PlaybackSection::Advanced, "display"));
    if (saved_)
    {
        SettingsCategory account {Tr("Compte"), "account", {}};
        for (const auto& line : {profile_.name, client_ ? client_->PlanLabel() : std::string()})
            if (!line.empty()) account.rows.push_back({line, {}, 3});
        account.rows.push_back({Tr("Se déconnecter"), {}, 2, -1, false, [this] { ConfirmSignOut(); }});
        categories.push_back(std::move(account));
    }
    SettingsCategory about {Tr("À propos"), "info", {}};
    about.rows.push_back({Tr("Vérifier les mises à jour"), {}, 2, -1, false, [this] { CheckUpdates(); }});
    about.rows.push_back({"LunaONE  " + VersionLabel(), {}, 3});
    about.rows.push_back({Tr("Crédit") + "  " + ProjectCredit, {}, 3});
    about.rows.push_back({Tr(Disclaimer), {}, 3});
    categories.push_back(std::move(about));
    return categories;
}

void LibraryScene::ShowSettings(int)
{
    if (busy_) return;
    if (panel_) ClosePanel();
    settings_ = true;
    settings_category_ = settings_column_ = settings_row_ = settings_first_ = 0;
    settings_editing_ = -1;
    settings_motion_ = 3;
    settings_motion_at_ = Clock::now();
    host_.Redraw();
}

void LibraryScene::CloseSettings()
{
    settings_ = false;
    host_.Redraw();
}

void LibraryScene::SettingsActivate(const SettingsEntry& entry, int row)
{
    settings_column_ = 1;
    settings_row_ = row;
    if (entry.kind == 2)
    {
        if (entry.action) entry.action();
        return;
    }
    if (entry.kind == 1)
    {
        ApplyChoice(entry.choice, entry.on ? 0 : 1);
        return;
    }
    const auto choices = PlaybackChoices(LoadPlaybackPreferences(), false, false, true);
    settings_editing_ = entry.choice;
    settings_value_ = static_cast<int>(choices.at(entry.choice).selected);
    settings_column_ = 2;
    settings_motion_ = 1;
    settings_motion_at_ = Clock::now();
}

void LibraryScene::SettingsMove(Move move)
{
    const auto categories = SettingsCategories();
    settings_category_ = std::clamp(settings_category_, 0, static_cast<int>(categories.size()) - 1);
    const auto& category = categories[settings_category_];
    std::vector<int> targets;
    for (std::size_t i = 0; i < category.rows.size(); ++i)
        if (category.rows[i].kind != 3) targets.push_back(static_cast<int>(i));
    const auto change_category = [&](int index) {
        index = std::clamp(index, 0, static_cast<int>(categories.size()) - 1);
        if (index == settings_category_) return;
        settings_category_ = index;
        settings_editing_ = -1;
        settings_first_ = 0;
        settings_motion_ = 3;
        settings_motion_at_ = Clock::now();
    };
    const auto step = [&](int direction) {
        auto at = std::ranges::find(targets, settings_row_);
        if (at == targets.end()) { if (!targets.empty()) settings_row_ = targets.front(); return; }
        if (direction < 0 && at != targets.begin()) settings_row_ = *(at - 1);
        if (direction > 0 && at + 1 != targets.end()) settings_row_ = *(at + 1);
    };
    const auto back_to_rows = [&] {
        settings_column_ = 1;
        settings_editing_ = -1;
        settings_motion_ = 2;
        settings_motion_at_ = Clock::now();
    };
    if (move == Move::Menu) { CloseSettings(); return; }
    if (move == Move::PreviousSection || move == Move::NextSection)
    {
        change_category(settings_category_ + (move == Move::NextSection ? 1 : -1));
        settings_column_ = 0;
        return;
    }
    // Column -1: the « Fermer » button, above the first category.
    if (settings_column_ == -1)
    {
        if (move == Move::Activate || move == Move::Back) { CloseSettings(); return; }
        if (move == Move::Down || move == Move::Left) settings_column_ = 0;
    }
    else if (settings_column_ == 0)
    {
        if (move == Move::Up && settings_category_ == 0) settings_column_ = -1;
        else if (move == Move::Up) change_category(settings_category_ - 1);
        else if (move == Move::Down) change_category(settings_category_ + 1);
        else if ((move == Move::Right || move == Move::Activate) && !targets.empty())
        {
            settings_column_ = 1;
            settings_row_ = targets.front();
        }
        else if (move == Move::Back || move == Move::Left) CloseSettings();
    }
    else if (settings_column_ == 1)
    {
        if (move == Move::Up) step(-1);
        else if (move == Move::Down) step(1);
        else if (move == Move::Left || move == Move::Back) settings_column_ = 0;
        else if (move == Move::Activate && settings_row_ < static_cast<int>(category.rows.size()))
            SettingsActivate(category.rows[settings_row_], settings_row_);
    }
    else
    {
        const auto choices = PlaybackChoices(LoadPlaybackPreferences(), false, false, true);
        const int count = settings_editing_ >= 0 ? static_cast<int>(choices.at(settings_editing_).labels.size()) : 0;
        if (move == Move::Up) settings_value_ = std::max(0, settings_value_ - 1);
        else if (move == Move::Down) settings_value_ = std::min(count - 1, settings_value_ + 1);
        else if (move == Move::Left || move == Move::Back) back_to_rows();
        else if (move == Move::Activate && settings_editing_ >= 0)
        {
            const int choice = settings_editing_;
            back_to_rows();
            ApplyChoice(choice, settings_value_);
        }
    }
}

void LibraryScene::DrawSettings()
{
    const auto categories = SettingsCategories();
    settings_category_ = std::clamp(settings_category_, 0, static_cast<int>(categories.size()) - 1);
    const auto& category = categories[settings_category_];
    const bool keys = device_ != InputDevice::Mouse;
    // Opaque, with the library's dark violet going darker downwards.
    Rect({0, 0, pixel_width_, pixel_height_}, 0xff100d1d);
    Paint shade;
    shade.kind = Paint::Kind::Shade;
    shade.box = {0, 0, pixel_width_, pixel_height_};
    paints_.push_back(shade);
    Clickable({0, 0, pixel_width_, pixel_height_}, [](int) {});
    Label(At(64, 32, 500, 50), Tr("Réglages"), 30, kText, Font::Bold);
    const bool close_focused = keys && settings_column_ == -1;
    Rect(At(width_ - 64 - 150, 34, 150, 46), close_focused ? 0x33aa88ff : kShell, 23, close_focused ? 3 : 1,
         close_focused ? kViolet : kRing);
    Label(At(width_ - 64 - 150, 34, 150, 46), Tr("Fermer"), 16, kText, Font::Bold, Align::Center);
    Clickable(At(width_ - 64 - 150, 34, 150, 46), [this](int) { CloseSettings(); });

    // Left: the categories.
    for (std::size_t i = 0; i < categories.size(); ++i)
    {
        const bool chosen = static_cast<int>(i) == settings_category_;
        const bool focused = chosen && keys && settings_column_ == 0;
        const float y = 112 + i * 62;
        Rect(At(64, y, 300, 54), chosen ? 0x33aa88ff : 0, 14, focused ? 2 : 0, kViolet);
        if (chosen) Rect(At(64, y + 14, 4, 26), kViolet, 2);
        Icon(At(84, y + 15, 24, 24), categories[i].icon, chosen ? kViolet : kText);
        Label(At(122, y, 230, 54), categories[i].title, 18, chosen ? kText : kSecondary, chosen ? Font::Bold : Font::Regular);
        Clickable(At(64, y, 300, 54), [this, i](int) {
            if (settings_category_ != static_cast<int>(i))
            {
                settings_motion_ = 3;
                settings_motion_at_ = Clock::now();
            }
            settings_category_ = static_cast<int>(i);
            settings_column_ = 0;
            settings_editing_ = -1;
            settings_first_ = 0;
        });
    }
    Rect(At(392, 112, 1, 540), 0x26ffffff);

    // Right: the rows of the category, or the values of the row being chosen.
    const float pane_x = 424, pane_width = width_ - pane_x - 64, pane_top = 104, pane_height = 548;
    const std::size_t pane_start = paints_.size();
    if (settings_editing_ >= 0)
    {
        const auto choices = PlaybackChoices(LoadPlaybackPreferences(), false, false, true);
        const auto& choice = choices.at(settings_editing_);
        Label(At(pane_x, pane_top + 8, pane_width, 44), "‹   " + choice.title, 22, kText, Font::Bold);
        Clickable(At(pane_x, pane_top + 8, pane_width, 44), [this](int) { SettingsMove(Move::Back); });
        const int visible = 7, count = static_cast<int>(choice.labels.size());
        int first = std::clamp(settings_value_ - visible + 1, 0, std::max(0, count - visible));
        first = std::min(first, settings_value_);
        for (int v = first; v < std::min(count, first + visible); ++v)
        {
            const bool current = static_cast<std::size_t>(v) == choice.selected;
            const bool focused = v == settings_value_ && keys;
            const auto box = At(pane_x, pane_top + 64 + (v - first) * 62, pane_width, 54);
            Rect(box, focused ? 0x33aa88ff : 0x12ffffff, 14, focused ? 2 : 0, kViolet);
            Rect(At(pane_x + 20, pane_top + 64 + (v - first) * 62 + 17, 20, 20), current ? kViolet : 0, 10, 2,
                 current ? kViolet : kRing);
            if (current) Rect(At(pane_x + 26, pane_top + 64 + (v - first) * 62 + 23, 8, 8), 0xffffffff, 4);
            Label(At(pane_x + 56, pane_top + 64 + (v - first) * 62, pane_width - 76, 54), choice.labels[v], 18,
                  current ? kText : kSecondary, current ? Font::Bold : Font::Regular);
            const int index = settings_editing_;
            Clickable(box, [this, v, index](int) {
                settings_value_ = v;
                settings_column_ = 1;
                settings_editing_ = -1;
                settings_motion_ = 2;
                settings_motion_at_ = Clock::now();
                ApplyChoice(index, v);
            });
        }
        if (first > 0) Label(At(pane_x + pane_width - 24, pane_top + 20, 20, 20), "▲", 12, kSecondary);
        if (first + visible < count)
            Label(At(pane_x + pane_width / 2 - 10, pane_top + pane_height - 22, 20, 20), "▼", 12, kSecondary);
    }
    else
    {
        Label(At(pane_x, pane_top + 8, pane_width, 44), category.title, 22, kText, Font::Bold);
        const auto height = [](const SettingsEntry& entry) { return entry.kind == 3 ? 40.f : 62.f; };
        const int rows = static_cast<int>(category.rows.size());
        // Keep the focused row inside the pane.
        if (settings_column_ == 1 && settings_row_ < rows)
        {
            settings_first_ = std::min(settings_first_, settings_row_);
            const auto fits = [&](int first) {
                float total = 0;
                for (int i = first; i <= settings_row_; ++i) total += height(category.rows[i]);
                return total <= pane_height - 64;
            };
            while (settings_first_ < settings_row_ && !fits(settings_first_)) ++settings_first_;
        }
        settings_first_ = std::clamp(settings_first_, 0, std::max(0, rows - 1));
        float y = 64;
        int shown_until = settings_first_;
        for (int i = settings_first_; i < rows && y + height(category.rows[i]) - 8 <= pane_height; ++i)
        {
            shown_until = i + 1;
            const auto& entry = category.rows[i];
            if (entry.kind == 3)
            {
                Label(At(pane_x + 20, pane_top + y, pane_width - 40, 40), entry.title, 15, kSecondary);
                y += 40;
                continue;
            }
            const bool focused = keys && settings_column_ == 1 && i == settings_row_;
            const auto box = At(pane_x, pane_top + y, pane_width, 54);
            Rect(box, focused ? 0x33aa88ff : 0x12ffffff, 14, focused ? 2 : 0, kViolet);
            const bool accent = entry.kind == 2;
            Label(At(pane_x + 20, pane_top + y, pane_width * .55f, 54), entry.title, 18, accent ? kViolet : kText,
                  entry.kind == 2 ? Font::Bold : Font::Regular);
            if (entry.kind == 1)
            {
                // A switch: violet and to the right when on.
                Rect(At(pane_x + pane_width - 76, pane_top + y + 12, 54, 30), entry.on ? kViolet : 0x40ffffff, 15);
                Rect(At(pane_x + pane_width - 76 + (entry.on ? 27 : 3), pane_top + y + 15, 24, 24), 0xffffffff, 12);
            }
            else if (entry.kind == 0)
                Label(At(pane_x + pane_width * .5f, pane_top + y, pane_width * .5f - 22, 54), entry.value + "   ›", 15,
                      kSecondary, Font::Regular, Align::Right);
            else
                Label(At(pane_x + pane_width - 60, pane_top + y, 40, 54), "›", 20, kViolet, Font::Bold, Align::Right);
            Clickable(box, [this, entry, i](int) { SettingsActivate(entry, i); });
            y += 62;
        }
        if (settings_first_ > 0) Label(At(pane_x + pane_width - 24, pane_top + 20, 20, 20), "▲", 12, kSecondary);
        if (shown_until < rows) Label(At(pane_x + pane_width / 2 - 10, pane_top + pane_height - 22, 20, 20), "▼", 12, kSecondary);
    }
    // The right pane slides in: from the right going deeper, from the left coming back.
    const float progress = Progress(settings_motion_at_, 220ms);
    if (progress < 1 && settings_motion_)
    {
        const float eased = 1 - (1 - progress) * (1 - progress) * (1 - progress);
        const float shift = (settings_motion_ == 1 ? 36.f : settings_motion_ == 2 ? -36.f : 0.f) * scale_ * (1 - eased);
        const float rise = (settings_motion_ == 3 ? 10.f : 0.f) * scale_ * (1 - eased);
        for (std::size_t i = pane_start; i < paints_.size(); ++i)
        {
            paints_[i].box.x += shift;
            paints_[i].box.y += rise;
            paints_[i].opacity *= eased;
        }
    }

    // Hints for the device in use.
    using input::PadButton;
    const bool swapped = LoadPlaybackPreferences().swap_ab;
    const std::string hint = device_ == InputDevice::Gamepad
        ? Name(swapped ? PadButton::B : PadButton::A) + "  " + Tr("Valider") + "    " +
              Name(swapped ? PadButton::A : PadButton::B) + "  " + Tr("Retour") + "    " + Name(PadButton::LeftShoulder) +
              " / " + Name(PadButton::RightShoulder) + "  " + Tr("Rubriques")
        : "↑ ↓ ← →  " + Tr("Naviguer") + "    ↵  " + Tr("Valider") + "    Esc  " + Tr("Retour");
    Label(At(64, 682, width_ - 128, 30), hint, 13, kSecondary);
}

void LibraryScene::ApplyChoice(std::size_t index, std::size_t value)
{
    auto preferences = LoadPlaybackPreferences();
    PlaybackChoices(preferences, false, false, true).at(index).apply(preferences, value);
    if (!SavePlaybackPreferences(preferences))
    {
        message_ = Tr("Impossible d’enregistrer les réglages vidéo.");
        return;
    }
    DeviceRegistration saved;
    SetUiLanguageForMarket(AmazonAuth::Load(saved) ? saved.market_code : preferences.market);
    if (!preferences.market.empty() && !saved_) market_ = preferences.market;
    if (host_.Fullscreen() != preferences.fullscreen) host_.SetFullscreen(preferences.fullscreen);
    if (simple_ != preferences.simple_library)
    {
        simple_ = preferences.simple_library;
        track_from_ = track_to_ = 0;
        if (client_) FollowSelection(true);
    }
}

void LibraryScene::ShowMarkets()
{
    std::vector<Row> rows;
    for (const auto& market : luna::AmazonMarkets())
    {
        const std::string code = market.code;
        rows.push_back({0, market.native_name, {}, code == market_, [this, code] {
            market_ = code;
            ClosePanel();
        }});
    }
    OpenPanel(Tr("Pays du compte"), std::move(rows), [this] { ClosePanel(); });
}

void LibraryScene::ConfirmSignOut()
{
    OpenPanel(Tr("Déconnecter ce compte de LunaONE ?"),
              {{4, Tr("Annuler"), {}, false, [this] { ClosePanel(); }},
               {4, Tr("Se déconnecter"), {}, false, [this] { ClosePanel(); SignOut(); }}},
              [this] { ClosePanel(); });
}

void LibraryScene::CheckUpdates()
{
    if (checking_updates_) return;
    checking_updates_ = true;
    OpenPanel(Tr("Vérifier les mises à jour"), {{3, Tr("Vérification des mises à jour…")}}, [this] { ClosePanel(); });
    std::thread([this, alive = alive_] {
        auto result = CheckForUpdates();
        host_.Post([this, alive, result] {
            if (!alive->load()) return;
            checking_updates_ = false;
            OpenPanel(Tr("Vérifier les mises à jour"),
                      {{3, result},
                       {4, Tr("Versions GitHub"), {}, false, [this] { host_.OpenUrl(ReleasesUrl); }},
                       {4, "OK", {}, false, [this] { ClosePanel(); }}},
                      [this] { ClosePanel(); });
        });
    }).detach();
}



void LibraryScene::Run(std::function<void()> job)
{
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread(std::move(job));
}

void LibraryScene::Cancel()
{
    if (!busy_) return;
    cancelled_ = true;
    message_ = Tr("Annulation…");
    host_.Redraw();
}

void LibraryScene::Connect()
{
    if (busy_) return;
    busy_ = true;
    cancelled_ = false;
    const bool saved = saved_;
    message_ = Tr(saved ? "Chargement du catalogue Luna…" : "Création du code Amazon…");
    loading_ = saved;
    loading_since_ = Clock::now();
    if (panel_) ClosePanel();
    host_.Redraw();
    Run([this, saved, market = market_, alive = alive_] {
        try
        {
            DeviceRegistration registration;
            if (!saved)
            {
                registration = AmazonAuth::SignInWithCode(market, [this, alive](const auto& code) {
                    host_.Post([this, alive, user_code = code.user_code, address = code.verification_url] {
                        if (!alive->load()) return;
                        code_ = user_code;
                        code_address_ = address;
                        message_ = Tr("Confirme ce code sur Amazon, puis reviens dans LunaONE.");
                        host_.Redraw();
                    });
                }, [this] { return cancelled_.load(); });
                if (cancelled_ || !AmazonAuth::Save(registration))
                {
                    try { AmazonAuth::Deregister(registration); } catch (const std::exception&) {}
                    throw std::runtime_error("Could not save sign-in.");
                }
            }
            else if (!AmazonAuth::Load(registration))
                throw std::runtime_error("Could not read the saved Amazon sign-in.");
            SetUiLanguageForMarket(registration.market_code);
            luna::CookieJar jar;
            jar.SetCookies(registration.cookies);
            auto client = std::make_shared<luna::LunaClient>(std::move(jar), std::move(registration));
            client->SignIn();
            auto shelves = client->FetchShelves();
            std::vector<Game> games;
            std::set<std::string> keys;
            for (const auto& shelf : shelves)
                for (const auto& game : shelf.games)
                    if (luna::InsertGameKeys(keys, game)) games.push_back(game);
            auto profile = client->Profile();
            host_.Post([this, alive, client, shelves = std::move(shelves), games = std::move(games),
                        profile = std::move(profile)]() mutable {
                if (!alive->load()) return;
                busy_ = loading_ = false;
                saved_ = true;
                code_.clear();
                client_ = std::move(client);
                shelves_ = std::move(shelves);
                catalog_ = std::move(games);
                profile_ = std::move(profile);
                message_.clear();
                zone_ = kCovers;
                OpenSection(2);
                ExploreQuietly();
            });
        }
        catch (const std::exception& error)
        {
            auto message = cancelled_ ? Tr("Opération annulée.") : std::string(error.what());
            host_.Post([this, alive, message] {
                if (!alive->load()) return;
                busy_ = loading_ = false;
                code_.clear();
                message_ = message;
                host_.Redraw();
            });
        }
    });
}

void LibraryScene::Search(std::string query)
{
    if (busy_ || !client_ || query.empty()) return;
    busy_ = true;
    cancelled_ = false;
    message_ = Tr("Recherche dans le catalogue Luna…");
    host_.Redraw();
    Run([this, query, client = client_, alive = alive_] {
        std::vector<Game> found;
        std::string error;
        try { found = client->SearchGames(query, [this] { return !cancelled_.load(); }); }
        catch (const std::exception& failure) { error = failure.what(); }
        host_.Post([this, alive, query, found = std::move(found), error]() mutable {
            if (!alive->load()) return;
            busy_ = false;
            if (cancelled_) message_ = Tr("Opération annulée.");
            else if (!error.empty()) message_ = error;
            else
            {
                shown_ = std::move(found);
                query_ = query;
                selected_ = 0;
                track_from_ = track_to_ = 0;
                zone_ = kCovers;
                message_.clear();
                FollowSelection(true);
            }
            host_.Redraw();
        });
    });
}

void LibraryScene::Explore()
{
    if (busy_ || !client_) return;
    busy_ = true;
    cancelled_ = false;
    message_ = Tr("Exploration du catalogue Luna…");
    host_.Redraw();
    std::set<std::string> known;
    for (const auto& game : catalog_) luna::InsertGameKeys(known, game);
    const std::size_t base = catalog_.size();
    Run([this, known = std::move(known), base, client = client_, alive = alive_]() mutable {
        std::vector<Game> found;
        std::string error;
        try
        {
            client->ExploreCatalog(std::move(known), [&](std::vector<Game> batch) {
                for (auto& game : batch) found.push_back(std::move(game));
                host_.Post([this, alive, count = base + found.size()] {
                    if (!alive->load()) return;
                    message_ = TrFormat("{} titres trouvés…", {std::to_string(count)});
                    host_.Redraw();
                });
            }, [this] { return !cancelled_.load(); });
        }
        catch (const std::exception& failure) { error = failure.what(); }
        host_.Post([this, alive, found = std::move(found), error]() mutable {
            if (!alive->load()) return;
            busy_ = false;
            for (auto& game : found) catalog_.push_back(std::move(game));
            message_ = error.empty() ? TrFormat("{} titres", {std::to_string(catalog_.size())}) : error;
            OpenSection(2);
        });
    });
}

// The home page lists only part of Luna: the linked pages are explored
// quietly after loading, and « Tous les jeux » grows as games arrive. A game
// stops the exploration until it ends.
void LibraryScene::ExploreQuietly()
{
    if (exploring_) exploring_->store(false);
    if (!client_) return;
    auto running = std::make_shared<std::atomic<bool>>(true);
    exploring_ = running;
    std::set<std::string> known;
    for (const auto& game : catalog_) luna::InsertGameKeys(known, game);
    std::thread([this, client = client_, running, known = std::move(known), alive = alive_]() mutable {
        try
        {
            client->ExploreCatalog(std::move(known), [&](std::vector<Game> batch) {
                host_.Post([this, client, alive, batch = std::move(batch)]() mutable {
                    if (alive->load()) AddExplored(client, std::move(batch));
                });
            }, [running, alive] { return running->load() && alive->load(); });
        }
        catch (const std::exception&) {}
    }).detach();
}

void LibraryScene::AddExplored(std::shared_ptr<luna::LunaClient> client, std::vector<Game> games)
{
    if (client != client_ || games.empty()) return;
    std::set<std::string> known;
    for (const auto& game : catalog_) luna::InsertGameKeys(known, game);
    for (auto& game : games)
        if (luna::InsertGameKeys(known, game)) catalog_.push_back(std::move(game));
    if (section_ != 2 || !query_.empty() || busy_) return;
    // The selected game stays selected while the list grows.
    const std::string asin = shown_.empty() ? std::string() : shown_.at(selected_).asin;
    shown_ = LibraryGames(LibrarySection::All, shelves_, catalog_, client_->Plan());
    for (std::size_t i = 0; i < shown_.size(); ++i)
        if (!asin.empty() && shown_[i].asin == asin) selected_ = i;
    first_ = selected_ / kPageSize * kPageSize;
    host_.Redraw();
}

void LibraryScene::SignOut()
{
    if (busy_) return;
    busy_ = true;
    message_ = Tr("Déconnexion…");
    host_.Redraw();
    Run([this, alive = alive_] {
        DeviceRegistration registration;
        if (AmazonAuth::Load(registration))
            try { AmazonAuth::Deregister(registration); } catch (const std::exception&) {}
        const bool erased = AmazonAuth::EraseAccountData();
        host_.Post([this, alive, erased] {
            if (!alive->load()) return;
            busy_ = false;
            if (!erased)
            {
                message_ = Tr("Impossible d’effacer la connexion enregistrée.");
                host_.Redraw();
                return;
            }
            SetUiLanguageForMarket(LoadPlaybackPreferences().market);
            client_.reset();
            catalog_.clear();
            shown_.clear();
            shelves_.clear();
            profile_ = {};
            saved_ = false;
            settings_ = false;
            message_.clear();
            backdrop_wanted_ = backdrop_shown_ = backdrop_previous_ = {};
            host_.Redraw();
        });
    });
}

void LibraryScene::PlaySelected()
{
    if (busy_ || !client_ || shown_.empty()) return;
    const auto game = shown_.at(selected_);
    if (PresentLibraryGame(game, client_->Plan()).locked)
    {
        message_ = Tr("Nécessite Luna Premium");
        host_.Redraw();
        return;
    }
    if (panel_) ClosePanel();
    // The game gets the whole connection.
    const bool exploring = exploring_ && exploring_->load();
    if (exploring_) exploring_->store(false);
    std::string result;
    try { host_.Play(*client_, game); }
    catch (const std::exception& error) { result = error.what(); }
    // Buttons still held from the exit chord must not act on the catalog.
    latched_ = true;
    message_ = result.empty() ? Tr("Partie fermée.") : result;
    if (exploring) ExploreQuietly();
    if (section_ == 1 && query_.empty())
    {
        const auto selected = selected_;
        OpenSection(1);
        selected_ = std::min(selected, shown_.empty() ? 0 : shown_.size() - 1);
        FollowSelection(true);
    }
    host_.Redraw();
}

} // namespace lunanx::ui
