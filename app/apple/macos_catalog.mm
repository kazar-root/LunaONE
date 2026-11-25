// SPDX-License-Identifier: GPL-3.0-only
// LunaONE for macOS. The library reproduces the Google TV screen
// (app/android/.../LibraryScreen.java): the same 1280 × 720 layout scaled to
// the window, colours, icons, carousel and backdrop. It is driven by the
// mouse, the keyboard or a controller. Playback is lunaone::apple::Play,
// unchanged.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#include "macos_player.hpp"
#include "macos_window.hpp"
#include "playback_support.hpp"
#include "../desktop/runtime.hpp"
#include "core/i18n.hpp"
#include "core/runtime_journal.hpp"
#include "core/playback_preferences.hpp"
#include "core/update_check.hpp"
#include "core/self_update.hpp"
#include "luna/amazon_auth.hpp"
#include "ui/device_qr.hpp"
#include "ui/image_url_policy.hpp"
#include "ui/library_sections.hpp"
#include "ui/virtual_keyboard.hpp"
#include "stream/launch_status.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <set>
#include <thread>

namespace
{
using namespace lunanx::luna;
using lunanx::ui::LibrarySection;

// LibraryScreen.java colours.
constexpr std::uint32_t kBackground = 0xff100d1d, kText = 0xfffaf8ff, kSecondary = 0xffc7bdd4, kViolet = 0xffaa88ff,
                        kGreen = 0xff78e3bd, kInk = 0xff151125, kShell = 0xe818142b, kCard = 0xff2a2240,
                        kRank = 0xff6f42ea, kQuiet = 0x8dd8cce8, kRing = 0x867d718e, kPill = 0xfff0eaff;
// The focus fill of the settings rows.
constexpr std::uint32_t kFocus = 0xff5640bb;
// The sign-in screen in the library's night colours: frosted cards on the
// logo's violet, violet buttons with a white ring on focus.
constexpr std::uint32_t kLoginCard = 0x1fffffff, kLoginCardEdge = 0x40aa88ff, kLoginButton = 0xff6a4cff,
                        kLoginButtonFocus = 0xff8466ff, kLoginSecondary = 0x1affffff,
                        kLoginSecondaryFocus = 0x55aa88ff, kLoginOutline = 0x4dffffff;
constexpr std::size_t kPageSize = 10;
const char* const kSections[] = {"Top 10", "Reprendre", "Tous les jeux", "Luna Premium"};

NSString* Text(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()] ?: @""; }
NSString* Label(const char* source) { return Text(lunanx::Tr(source)); }

NSColor* Argb(std::uint32_t argb)
{
    return [NSColor colorWithSRGBRed:((argb >> 16) & 255) / 255.0 green:((argb >> 8) & 255) / 255.0
                                blue:(argb & 255) / 255.0 alpha:((argb >> 24) & 255) / 255.0];
}

bool AmazonImage(NSURL* url)
{
    return [url.scheme isEqualToString:@"https"] &&
           ([url.host hasSuffix:@".media-amazon.com"] || [url.host hasSuffix:@".images-amazon.com"]);
}

NSImage* QrImage(const std::string& url)
{
    const auto qr = lunanx::ui::MakeDeviceQr(url);
    if (!qr.size) return nil;
    const int scale = 6, side = scale * qr.size;
    auto rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:side pixelsHigh:side
                      bitsPerSample:8 samplesPerPixel:1 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceWhiteColorSpace
                      bytesPerRow:side bitsPerPixel:8];
    if (!rep) return nil;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            rep.bitmapData[y * rep.bytesPerRow + x] = qr.dark[(y / scale) * qr.size + x / scale] ? 0 : 255;
    auto image = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    [image addRepresentation:rep];
    return image;
}

enum class Move { Left, Right, Up, Down, Activate, Back, Menu, Search, Space, PreviousSection, NextSection };
enum class Device { Mouse, Keyboard, Gamepad };
// Focus zones of the library, top to bottom: header and section icons,
// covers, then the buttons under the details.
enum Zone : int { kTop = 0, kCovers = 1, kActions = 2 };
enum Action : int { kPlay = 0, kPrevious = 1, kNext = 2, kCancel = 3 };
} // namespace

// Artwork loads from Amazon's image hosts only, cached for the session.
@interface LunaArtwork : NSObject
+ (instancetype)shared;
- (void)load:(NSString*)address completion:(void (^)(NSImage*))completion;
@end

@implementation LunaArtwork
{
    NSCache<NSString*, NSImage*>* _cache;
    NSURLSession* _session;
}
+ (instancetype)shared
{
    static LunaArtwork* artwork = [LunaArtwork new];
    return artwork;
}
- (instancetype)init
{
    if (!(self = [super init])) return nil;
    _cache = [NSCache new];
    _cache.countLimit = 240;
    auto config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.HTTPMaximumConnectionsPerHost = 4;
    config.HTTPShouldSetCookies = NO;
    config.timeoutIntervalForRequest = 8;
    _session = [NSURLSession sessionWithConfiguration:config];
    return self;
}
- (void)load:(NSString*)address completion:(void (^)(NSImage*))completion
{
    if (address.length == 0) return;
    if (NSImage* cached = [_cache objectForKey:address]) { completion(cached); return; }
    NSURL* url = [NSURL URLWithString:address];
    if (!AmazonImage(url)) return;
    [[_session dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        if (error || data.length == 0 || data.length > 8 * 1024 * 1024 ||
            [(NSHTTPURLResponse*)response statusCode] != 200) return;
        NSImage* image = [[NSImage alloc] initWithData:data];
        if (!image) return;
        [self->_cache setObject:image forKey:address];
        dispatch_async(dispatch_get_main_queue(), ^{ completion(image); });
    }] resume];
}
@end

@interface LunaFlippedView : NSView
@end
@implementation LunaFlippedView
- (BOOL)isFlipped { return YES; }
@end

// An image filling its view like Android's CENTER_CROP.
@interface LunaImageView : LunaFlippedView
@property(nonatomic, strong) NSImage* image;
@property(nonatomic, copy) NSString* key;
@end
@implementation LunaImageView
- (instancetype)initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    self.layer.contentsGravity = kCAGravityResizeAspectFill;
    self.layer.masksToBounds = YES;
    return self;
}
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer
{
    self.layer.contentsGravity = kCAGravityResizeAspectFill;
    self.layer.contents = (__bridge id)[self.image CGImageForProposedRect:nullptr context:nil hints:nil];
}
- (void)setImage:(NSImage*)image
{
    _image = image;
    self.needsDisplay = YES;
}
- (NSView*)hitTest:(NSPoint)point { return nil; }
- (void)show:(NSString*)address
{
    if ([self.key isEqualToString:address]) return;
    self.key = address;
    self.image = nil;
    __weak LunaImageView* view = self;
    [[LunaArtwork shared] load:address completion:^(NSImage* image) {
        if ([view.key isEqualToString:address]) view.image = image;
    }];
}
@end

// LibraryScreen.Icon: 24 × 24 line icons.
@interface LunaIcon : LunaFlippedView
@property(nonatomic, copy) NSString* type;
@property(nonatomic, strong) NSColor* color;
@end
@implementation LunaIcon
- (NSView*)hitTest:(NSPoint)point { return nil; }
- (void)drawRect:(NSRect)dirty
{
    const CGFloat side = std::min(self.bounds.size.width, self.bounds.size.height);
    auto transform = [NSAffineTransform transform];
    [transform translateXBy:(self.bounds.size.width - side) / 2 yBy:(self.bounds.size.height - side) / 2];
    [transform scaleBy:side / 24];
    auto path = [NSBezierPath bezierPath];
    path.lineWidth = 1.85;
    path.lineCapStyle = NSLineCapStyleRound;
    path.lineJoinStyle = NSLineJoinStyleRound;
    const auto line = [&](std::initializer_list<CGFloat> points) {
        auto value = points.begin();
        [path moveToPoint:NSMakePoint(value[0], value[1])];
        for (std::size_t i = 2; i + 1 < points.size(); i += 2)
            [path lineToPoint:NSMakePoint(value[i], value[i + 1])];
    };
    const auto circle = [&](CGFloat x, CGFloat y, CGFloat r) {
        [path appendBezierPathWithOvalInRect:NSMakeRect(x - r, y - r, 2 * r, 2 * r)];
    };
    const auto rounded = [&](CGFloat l, CGFloat t, CGFloat r, CGFloat b) {
        [path appendBezierPathWithRoundedRect:NSMakeRect(l, t, r - l, b - t) xRadius:2 yRadius:2];
    };
    // An arc of the ellipse inscribed in a rectangle, angles as Android draws them.
    const auto arc = [&](CGFloat l, CGFloat t, CGFloat r, CGFloat b, CGFloat start, CGFloat sweep) {
        const CGFloat cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
        for (int step = 0; step <= 24; ++step)
        {
            const CGFloat angle = (start + sweep * step / 24) * M_PI / 180;
            const NSPoint point = NSMakePoint(cx + rx * std::cos(angle), cy + ry * std::sin(angle));
            if (step == 0) [path moveToPoint:point];
            else [path lineToPoint:point];
        }
    };
    if ([self.type isEqualToString:@"clock"]) { circle(12, 12, 8); line({12, 7, 12, 12, 15, 13}); }
    else if ([self.type isEqualToString:@"search"]) { circle(10, 10, 5.8); line({14.5, 14.5, 20, 20}); }
    else if ([self.type isEqualToString:@"grid"])
    {
        for (CGFloat x : {4, 13})
            for (CGFloat y : {4, 13}) rounded(x, y, x + 7, y + 7);
    }
    else if ([self.type isEqualToString:@"trophy"])
    {
        rounded(8, 4, 16, 14);
        line({8, 6, 4, 6, 5, 10, 8, 11});
        line({16, 6, 20, 6, 19, 10, 16, 11});
        line({12, 14, 12, 20});
        line({8, 20, 16, 20});
    }
    else if ([self.type isEqualToString:@"account"]) { circle(12, 8, 3.5); arc(5, 13, 19, 25, 186, 168); }
    else if ([self.type isEqualToString:@"settings"])
    {
        circle(12, 12, 6);
        circle(12, 12, 2.1);
        for (int i = 0; i < 8; ++i)
        {
            const CGFloat angle = i * M_PI / 4;
            [path moveToPoint:NSMakePoint(12 + 9 * std::sin(angle), 12 - 9 * std::cos(angle))];
            [path lineToPoint:NSMakePoint(12 + 6 * std::sin(angle), 12 - 6 * std::cos(angle))];
        }
    }
    else if ([self.type isEqualToString:@"lock"]) { rounded(6, 10, 18, 20); arc(8, 4, 16, 14, 180, 180); }
    else if ([self.type isEqualToString:@"gamepad"])
    {
        [path appendBezierPathWithRoundedRect:NSMakeRect(3, 7, 18, 11) xRadius:5 yRadius:5];
        line({7, 12.5, 10, 12.5});
        line({8.5, 11, 8.5, 14});
        circle(15.5, 11.5, .7);
        circle(17.5, 13.5, .7);
    }
    else if ([self.type isEqualToString:@"display"])
    {
        [path appendBezierPathWithRoundedRect:NSMakeRect(3, 5, 18, 11) xRadius:2 yRadius:2];
        line({12, 16, 12, 19});
        line({8, 19.5, 16, 19.5});
    }
    else if ([self.type isEqualToString:@"info"])
    {
        circle(12, 12, 8.5);
        line({12, 11, 12, 16.5});
        circle(12, 7.8, .5);
    }
    [path transformUsingAffineTransform:transform];
    [self.color setStroke];
    [path stroke];
}
@end

// A clickable element. Its labels and images never take the click.
@interface LunaControl : LunaFlippedView
@property(nonatomic, copy) void (^clicked)(NSInteger clicks);
@property(nonatomic, copy) void (^decorate)(BOOL focused);
@property(nonatomic) BOOL disabled;
@end
@implementation LunaControl
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
// The innermost control under the pointer takes the click; text fields keep it.
- (NSView*)hitTest:(NSPoint)point
{
    NSView* hit = [super hitTest:point];
    for (NSView* view = hit; view && view != self; view = view.superview)
    {
        if ([view isKindOfClass:NSTextField.class] && ((NSTextField*)view).editable) return hit;
        if ([view isKindOfClass:LunaControl.class]) return view;
    }
    return hit ? self : nil;
}
- (void)mouseDown:(NSEvent*)event {}
- (void)mouseUp:(NSEvent*)event
{
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (!self.disabled && self.clicked && NSPointInRect(point, self.bounds)) self.clicked(event.clickCount);
}
- (void)setDisabled:(BOOL)disabled
{
    _disabled = disabled;
    self.alphaValue = disabled ? .55 : 1;
}
@end

// The window content: forwards the wheel, the pointer and size changes to the catalog.
@interface LunaRootView : LunaFlippedView
@property(nonatomic, copy) void (^wheelHandler)(NSEvent* event);
@property(nonatomic, copy) void (^pointerMoved)(void);
@property(nonatomic, copy) void (^resized)(void);
@end
@implementation LunaRootView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event
{
    // Keys are read by the catalog's event monitor; others are ignored quietly.
}
- (void)scrollWheel:(NSEvent*)event
{
    if (self.wheelHandler) self.wheelHandler(event);
}
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    if (self.resized) self.resized();
}
- (void)updateTrackingAreas
{
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)mouseMoved:(NSEvent*)event
{
    if (self.pointerMoved) self.pointerMoved();
}
@end

// Selected game artwork behind the library, under LibraryScreen's shade.
@interface LunaBackdrop : NSView
@property(nonatomic, strong) CALayer* artwork;
@property(nonatomic, strong) CAGradientLayer* shade;
@property(nonatomic, copy) NSString* key;
@end
@implementation LunaBackdrop
- (instancetype)initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    self.layer.backgroundColor = Argb(kBackground).CGColor;
    self.layer.masksToBounds = YES;
    self.artwork = [CALayer layer];
    self.artwork.contentsGravity = kCAGravityResizeAspectFill;
    [self.layer addSublayer:self.artwork];
    self.shade = [CAGradientLayer layer];
    // Top to bottom, as the GradientDrawable of LibraryScreen.
    self.shade.startPoint = CGPointMake(.5, 1);
    self.shade.endPoint = CGPointMake(.5, 0);
    self.shade.colors = @[(__bridge id)Argb(0xbd110f1d).CGColor, (__bridge id)Argb(0x55100d1c).CGColor,
                          (__bridge id)Argb(0x28100c17).CGColor, (__bridge id)Argb(0xd8090612).CGColor,
                          (__bridge id)Argb(0xff090611).CGColor];
    [self.layer addSublayer:self.shade];
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    return self;
}
- (void)layout
{
    [super layout];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.artwork.frame = self.shade.frame = self.bounds;
    [CATransaction commit];
}
- (NSView*)hitTest:(NSPoint)point { return nil; }
// The previous picture stays until the next one has loaded, then cross-fades.
- (void)show:(NSString*)address
{
    if ([self.key isEqualToString:address]) return;
    self.key = address;
    __weak LunaBackdrop* view = self;
    [[LunaArtwork shared] load:address completion:^(NSImage* image) {
        LunaBackdrop* backdrop = view;
        if (![backdrop.key isEqualToString:address]) return;
        auto fade = [CATransition animation];
        fade.type = kCATransitionFade;
        fade.duration = NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion ? 0 : .35;
        [backdrop.artwork addAnimation:fade forKey:@"artwork"];
        backdrop.artwork.contents = image;
    }];
}
@end

@interface LunaPanelRow : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* value;
// 0 action, 1 section heading, 2 information, 3 paragraph, 4 button at the
// bottom: the first one on the left, the others on the right, the last
// being the main action.
@property(nonatomic) NSInteger kind;
@property(nonatomic) BOOL checked;
@property(nonatomic, copy) void (^action)(void);
@end
@implementation LunaPanelRow
@end

namespace
{
LunaPanelRow* Row(NSString* title, NSString* value, void (^action)(void))
{
    auto row = [LunaPanelRow new];
    row.title = title;
    row.value = value;
    row.action = action;
    return row;
}
LunaPanelRow* Button(NSString* title, void (^action)(void))
{
    auto row = Row(title, nil, action);
    row.kind = 4;
    return row;
}
LunaPanelRow* Note(NSString* title, NSInteger kind)
{
    auto row = [LunaPanelRow new];
    row.title = title;
    row.kind = kind;
    return row;
}
} // namespace

namespace
{
// A row of the settings screen.
struct SettingsEntry
{
    NSString* title = nil;
    NSString* value = nil;
    // 0 choice with a list of values, 1 switch, 2 action, 3 note.
    int kind = 0;
    int choice = -1;
    bool on = false;
    void (^action)(void) = nil;
};

struct SettingsCategory
{
    NSString* title;
    NSString* icon;
    std::vector<SettingsEntry> rows;
};
} // namespace

@interface LunaCatalog : NSObject<NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate>
@end

@implementation LunaCatalog
{
    LunaWindow* _window;
    LunaRootView* _root;
    LunaBackdrop* _backdrop;
    LunaFlippedView* _content;
    NSView* _panelView;
    NSOperationQueue* _jobs;
    std::shared_ptr<LunaClient> _client;
    std::vector<Shelf> _shelves;
    std::vector<Game> _catalog, _shown;
    PlayerProfile _profile;
    std::atomic_bool _cancelled;
    bool _busy, _saved, _simple;
    std::string _query, _market;
    int _section;
    std::size_t _selected, _first;
    NSString* _message;
    // Design canvas: LibraryScreen's 1280 × 720 points, scaled to the window.
    CGFloat _scale, _width, _offsetY;
    // The sign-in screen centres its 1280-wide design; the library uses the full width.
    CGFloat _offsetX;
    // Library focus.
    Zone _zone;
    int _topIndex, _actionIndex;
    std::vector<LunaControl*> _top, _covers;
    LunaControl* _actions[4];
    LunaFlippedView* _track;
    CGFloat _trackOffset, _trackLength;
    NSTextField *_title, *_offer, *_metadata, *_status, *_hints, *_system;
    // Sign-in screen.
    std::vector<LunaControl*> _loginControls;
    std::size_t _loginFocus;
    NSString *_code, *_codeAddress;
    NSImage* _qr;
    // Panels: menu, settings, choices, search, messages.
    NSString* _panelTitle;
    NSArray<LunaPanelRow*>* _panelRows;
    NSInteger _panelFocus, _panelFirst;
    void (^_panelBack)(void);
    NSTextField* _panelField;
    // Settings screen: column 0 categories, 1 rows, 2 values of a row.
    BOOL _settings;
    // The same screen shows the main menu (1) or the settings (0).
    int _settingsCategory, _settingsColumn, _settingsRow, _settingsValue, _settingsEditing, _settingsFirst;
    // Pane entrance: 1 deeper, 2 back, 3 another category.
    int _settingsMotion;
    NSView* _settingsView;
    // On-screen keyboard of the search panel (L1 + L2 + R1 + R2 or LB + LT + RB + RT).
    std::unique_ptr<lunanx::ui::VirtualKeyboard> _keyboard;
    // Loading screen while the catalog is read.
    bool _loading;
    std::vector<NSView*> _pulse;
    NSTimer* _pulseTimer;
    NSTimeInterval _loadingSince;
    // Input.
    Device _device;
    NSTimer* _gamepadTimer;
    std::uint32_t _pad;
    bool _padLatched, _chordHeld;
    std::uint32_t _shoulders;
    int _repeatMove;
    NSTimeInterval _repeatAt;
    NSTimer* _clock;
    std::unique_ptr<lunaone::apple::NetworkStatus> _network;
    BOOL _checkingUpdates;
    // Quiet exploration of the whole catalog after loading.
    std::shared_ptr<std::atomic_bool> _exploring;
}

#pragma mark - Application

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    [self buildMenu];
    _jobs = [NSOperationQueue new];
    _jobs.maxConcurrentOperationCount = 1;
    _network = std::make_unique<lunaone::apple::NetworkStatus>();
    _market = "fr";
    _section = 2;
    _zone = kCovers;
    _device = Device::Mouse;
    _window = [[LunaWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                  NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"LunaONE";
    _window.contentMinSize = NSMakeSize(960, 540);
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _window.backgroundColor = Argb(kBackground);
    _window.delegate = self;
    // The green button and ⌃⌘F use the borderless full screen (LunaWindow).
    __weak LunaCatalog* weakSelf = self;
    _window.fullscreenChanged = ^(BOOL fullscreen) {
        auto preferences = lunanx::LoadPlaybackPreferences();
        preferences.fullscreen = fullscreen;
        lunanx::SavePlaybackPreferences(preferences);
        [weakSelf refreshHints];
    };
    _root = [[LunaRootView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    _root.wantsLayer = YES;
    _backdrop = [[LunaBackdrop alloc] initWithFrame:_root.bounds];
    [_root addSubview:_backdrop];
    _window.contentView = _root;
    __weak LunaCatalog* weak = self;
    _root.wheelHandler = ^(NSEvent* event) { [weak wheel:event]; };
    _root.pointerMoved = ^{ [weak usedDevice:Device::Mouse]; };
    _root.resized = ^{
        LunaCatalog* catalog = weak;
        if (!catalog) return;
        [NSObject cancelPreviousPerformRequestsWithTarget:catalog selector:@selector(render) object:nil];
        [catalog performSelector:@selector(render) withObject:nil afterDelay:0];
    };

    DeviceRegistration registration;
    _saved = AmazonAuth::Load(registration);
    _loading = _saved;
    const auto preferences = lunanx::LoadPlaybackPreferences();
    _simple = preferences.simple_library;
    if (!preferences.market.empty()) _market = preferences.market;
    lunanx::SetUiLanguageForMarket(_saved ? registration.market_code : preferences.market);
    [_window center];
    if (NSProcessInfo.processInfo.environment[@"LUNAONE_SNAPSHOT"]) [_window orderBack:nil];
    else [_window makeKeyAndOrderFront:nil];
    lunaone::apple::ReportDeviceDisplay(_window.screen);
    if (preferences.fullscreen) lunaone::apple::SetBorderlessFullscreen(_window, true);
    // A snapshot run stays in the background and ignores controllers.
    const bool snapshots = NSProcessInfo.processInfo.environment[@"LUNAONE_SNAPSHOT"] != nil;
    if (!snapshots) [NSApp activateIgnoringOtherApps:YES];
    [self render];
    [_window makeFirstResponder:_root];
    // Keys reach the catalog whatever holds the focus, except while text is typed.
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        LunaCatalog* catalog = weak;
        if (!catalog || event.window != catalog->_window || NSApp.modalWindow) return event;
        const bool typing = [catalog->_window.firstResponder isKindOfClass:NSText.class];
        if (typing && !lunaone::apple::IsFullscreenShortcut(event)) return event;
        return [catalog key:event] ? nil : event;
    }];
    if (!snapshots)
        _gamepadTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(pollGamepad)
                                                       userInfo:nil repeats:YES];
    _clock = [NSTimer scheduledTimerWithTimeInterval:30 target:self selector:@selector(updateSystem)
                                            userInfo:nil repeats:YES];
    if (_saved) [self connect];
    // Development aid: LUNAONE_SNAPSHOT=<file.png> writes the window to that
    // file every second, to check the layout without screen recording rights;
    // LUNAONE_SNAPSHOT_PANEL=menu|settings|markets|keyboard|code opens that panel first.
    if (NSProcessInfo.processInfo.environment[@"LUNAONE_SNAPSHOT"])
    {
        [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(snapshot) userInfo:nil repeats:YES];
        NSString* panel = NSProcessInfo.processInfo.environment[@"LUNAONE_SNAPSHOT_PANEL"];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            self->_device = Device::Keyboard;
            if ([panel hasPrefix:@"settings"])
            {
                // settings:<category>:<column>:<row>, the row opening its values in column 2.
                [self showSettings:-1];
                NSArray<NSString*>* parts = [panel componentsSeparatedByString:@":"];
                if (parts.count == 4)
                {
                    self->_settingsCategory = parts[1].intValue;
                    self->_settingsColumn = std::min(parts[2].intValue, 1);
                    self->_settingsRow = parts[3].intValue;
                    if (parts[2].intValue == 2)
                        [self settingsActivate:[self settingsCategories][self->_settingsCategory].rows[self->_settingsRow]
                                           row:self->_settingsRow];
                    [self render];
                }
            }
            else if ([panel isEqualToString:@"markets"]) [self showMarkets];
            else if ([panel isEqualToString:@"code"])
            {
                // A made-up code, to check the code card without asking Amazon for one.
                self->_code = @"LUNA42";
                self->_codeAddress = @"https://www.amazon.fr/code";
                self->_qr = QrImage("https://www.amazon.fr/code");
                [self render];
            }
        });
        if ([panel isEqualToString:@"fullscreen-cycle"])
            for (int step = 0; step < 4; ++step)
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (2 + step) * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                    if (step < 3) [self toggleFullscreen:nil];
                    NSLog(@"CYCLE step=%d full=%d frame=%@ screen=%@ style=%lu", step,
                          lunaone::apple::IsBorderlessFullscreen(self->_window), NSStringFromRect(self->_window.frame),
                          NSStringFromRect(self->_window.screen.frame), (unsigned long)self->_window.styleMask);
                });
        if ([panel isEqualToString:@"keyboard"])
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                self->_device = Device::Gamepad;
                self->_panelField = [NSTextField textFieldWithString:@"Sonic"];
                self->_keyboard = std::make_unique<lunanx::ui::VirtualKeyboard>(lunanx::UiLanguage());
                self->_keyboard->Move(0, 1);
                [self openPanel:Label("Rechercher un jeu") rows:@[Button(Label("Annuler"), ^{ [self closePanel]; }),
                    Button(Label("Clavier"), ^{}), Button(Label("Rechercher"), ^{})]
                           back:^{ [self closePanel]; }];
                self->_panelField.target = self;
                self->_panelField.action = @selector(submitSearch:);
                self->_panelField.cell.sendsActionOnEndEditing = NO;
                [self editField];
                // Three on-screen keys, as a controller would press them.
                for (int i = 0; i < 3; ++i) [self keyboardMove:Move::Activate];
            });
    }
}

- (void)snapshot
{
    NSString* path = NSProcessInfo.processInfo.environment[@"LUNAONE_SNAPSHOT"];
    const NSSize size = _root.bounds.size;
    const CGFloat scale = _window.backingScaleFactor;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, size.width * scale, size.height * scale, 8, 0, space,
                                                 kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (!context) return;
    CGContextTranslateCTM(context, 0, size.height * scale);
    CGContextScaleCTM(context, scale, -scale);
    [_root.layer renderInContext:context];
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (!image) return;
    auto bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease(image);
    [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }
- (void)applicationWillTerminate:(NSNotification*)notification { _cancelled = true; }

- (void)windowDidChangeScreen:(NSNotification*)notification
{
    lunaone::apple::ReportDeviceDisplay(_window.screen);
}


- (void)buildMenu
{
    auto menu = [NSMenu new];
    const auto submenu = [&](NSString* title) {
        auto item = [NSMenuItem new];
        item.submenu = [[NSMenu alloc] initWithTitle:title];
        [menu addItem:item];
        return item.submenu;
    };
    auto application = submenu(@"LunaONE");
    [application addItemWithTitle:Label("Quitter LunaONE") action:@selector(terminate:) keyEquivalent:@"q"];
    // Copy and paste in the search field.
    auto edit = submenu(Label("Édition"));
    [edit addItemWithTitle:Label("Couper") action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:Label("Copier") action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:Label("Coller") action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:Label("Tout sélectionner") action:@selector(selectAll:) keyEquivalent:@"a"];
    auto view = submenu(Label("Affichage"));
    auto full = [view addItemWithTitle:Label("Plein écran") action:@selector(toggleFullscreen:) keyEquivalent:@"f"];
    full.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    full.target = self;
    auto window = submenu(Label("Fenêtre"));
    [window addItemWithTitle:Label("Réduire") action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window addItemWithTitle:Label("Fermer") action:@selector(performClose:) keyEquivalent:@"w"];
    NSApp.mainMenu = menu;
}

- (void)toggleFullscreen:(id)sender
{
    [_window toggleFullScreen:nil];
}

#pragma mark - Layout helpers

- (NSRect)x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w h:(CGFloat)h
{
    return NSMakeRect(_offsetX + x * _scale, _offsetY + y * _scale, std::max<CGFloat>(0, w * _scale),
                      std::max<CGFloat>(0, h * _scale));
}

- (NSRect)local:(CGFloat)x y:(CGFloat)y w:(CGFloat)w h:(CGFloat)h
{
    return NSMakeRect(x * _scale, y * _scale, std::max<CGFloat>(0, w * _scale), std::max<CGFloat>(0, h * _scale));
}

- (NSFont*)font:(CGFloat)size bold:(BOOL)bold
{
    return bold ? [NSFont systemFontOfSize:size * _scale weight:NSFontWeightBold] : [NSFont systemFontOfSize:size * _scale];
}

- (NSTextField*)label:(NSString*)text size:(CGFloat)size color:(std::uint32_t)color bold:(BOOL)bold
{
    auto label = [NSTextField labelWithString:text ?: @""];
    label.font = [self font:size bold:bold];
    label.textColor = Argb(color);
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.maximumNumberOfLines = 1;
    return label;
}

// A one-line label vertically centred in a frame, as Android's CENTER_VERTICAL.
- (NSTextField*)put:(NSTextField*)label in:(NSView*)parent frame:(NSRect)frame
{
    const CGFloat height = std::ceil(label.cell.cellSize.height);
    label.frame = NSMakeRect(frame.origin.x, frame.origin.y + (frame.size.height - height) / 2, frame.size.width, height);
    [parent addSubview:label];
    return label;
}

- (NSTextField*)paragraph:(NSString*)text size:(CGFloat)size color:(std::uint32_t)color bold:(BOOL)bold
                       in:(NSView*)parent frame:(NSRect)frame centred:(BOOL)centred
{
    auto label = [NSTextField wrappingLabelWithString:text ?: @""];
    label.font = [self font:size bold:bold];
    label.textColor = Argb(color);
    label.alignment = centred ? NSTextAlignmentCenter : NSTextAlignmentLeft;
    label.maximumNumberOfLines = 0;
    const CGFloat height = std::min(frame.size.height,
        std::ceil([label.cell cellSizeForBounds:NSMakeRect(0, 0, frame.size.width, CGFLOAT_MAX)].height));
    label.frame = NSMakeRect(frame.origin.x, frame.origin.y + (centred ? (frame.size.height - height) / 2 : 0),
                             frame.size.width, height);
    [parent addSubview:label];
    return label;
}

- (NSView*)shape:(std::uint32_t)color radius:(CGFloat)radius stroke:(CGFloat)stroke strokeColor:(std::uint32_t)strokeColor
           frame:(NSRect)frame in:(NSView*)parent
{
    NSView* view = [[LunaFlippedView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    [self paint:view color:color radius:radius stroke:stroke strokeColor:strokeColor];
    [parent addSubview:view];
    return view;
}

- (void)paint:(NSView*)view color:(std::uint32_t)color radius:(CGFloat)radius stroke:(CGFloat)stroke
  strokeColor:(std::uint32_t)strokeColor
{
    view.wantsLayer = YES;
    view.layer.backgroundColor = Argb(color).CGColor;
    view.layer.cornerRadius = radius * _scale;
    view.layer.borderWidth = stroke * _scale;
    view.layer.borderColor = Argb(strokeColor).CGColor;
}

- (LunaControl*)control:(NSRect)frame in:(NSView*)parent clicked:(void (^)(NSInteger))clicked
{
    auto control = [[LunaControl alloc] initWithFrame:frame];
    control.wantsLayer = YES;
    control.clicked = clicked;
    [parent addSubview:control];
    return control;
}

// LibraryScreen.pill: rounded buttons, white for the main action.
- (LunaControl*)pill:(NSString*)title primary:(BOOL)primary frame:(NSRect)frame action:(void (^)(void))action
{
    auto button = [self control:frame in:_content clicked:^(NSInteger) { action(); }];
    auto label = [self label:title size:primary ? 20 : 17 color:primary ? kInk : kText bold:YES];
    label.alignment = NSTextAlignmentCenter;
    [self put:label in:button frame:button.bounds];
    label.autoresizingMask = NSViewWidthSizable;
    __weak LunaCatalog* weak = self;
    __weak LunaControl* weakButton = button;
    button.decorate = ^(BOOL focused) {
        [weak paint:weakButton color:primary ? kPill : kShell radius:26 stroke:focused ? 3 : 0 strokeColor:kViolet];
    };
    button.decorate(NO);
    return button;
}

#pragma mark - Rendering

- (void)render
{
    const NSSize size = _root.bounds.size;
    if (size.width < 1 || size.height < 1) return;
    _scale = std::min(size.width / 1280, size.height / 720);
    _width = size.width / _scale;
    _offsetY = (size.height - 720 * _scale) / 2;
    _offsetX = 0;
    [_content removeFromSuperview];
    _content = [[LunaFlippedView alloc] initWithFrame:_root.bounds];
    [_root addSubview:_content positioned:NSWindowAbove relativeTo:_backdrop];
    _top.clear();
    _covers.clear();
    _loginControls.clear();
    std::fill(std::begin(_actions), std::end(_actions), nil);
    _system = _status = _hints = _title = _offer = _metadata = nil;
    _track = nil;
    _pulse.clear();
    if (_loading) [self renderLoading];
    else if (_client) [self renderLibrary];
    else [self renderLogin];
    [self renderSettings];
    [self renderPanel];
}

// While the catalog loads: logo, status and the three squares of the
// launch screen, with a cancel button.
- (void)renderLoading
{
    auto logo = [[LunaImageView alloc] initWithFrame:[self x:_width / 2 - 48 y:208 w:96 h:96]];
    logo.image = NSApp.applicationIconImage;
    [_content addSubview:logo];
    auto name = [self label:@"LunaONE" size:34 color:kText bold:YES];
    name.alignment = NSTextAlignmentCenter;
    [self put:name in:_content frame:[self x:0 y:322 w:_width h:48]];
    _status = [self label:_message.length ? _message : Label("Chargement…") size:19 color:kSecondary bold:NO];
    _status.alignment = NSTextAlignmentCenter;
    [self put:_status in:_content frame:[self x:80 y:378 w:_width - 160 h:32]];
    for (int i = 0; i < 3; ++i)
        _pulse.push_back([self shape:0xffffffff radius:2 stroke:0 strokeColor:0
                               frame:[self x:_width / 2 - 26.5 + i * 21 y:428 w:11 h:11] in:_content]);
    __weak LunaCatalog* weak = self;
    _actions[kCancel] = [self pill:Label("Annuler") primary:NO frame:[self x:_width / 2 - 67 y:476 w:134 h:52]
                            action:^{ [weak cancelJob]; }];
    _actions[kCancel].disabled = _cancelled.load();
    _actions[kCancel].decorate(_device != Device::Mouse);
    _hints = nil;
    [self pulse];
    if (!_pulseTimer)
        _pulseTimer = [NSTimer scheduledTimerWithTimeInterval:.1 target:self selector:@selector(pulse) userInfo:nil repeats:YES];
}

- (void)pulse
{
    if (!_loading || _pulse.empty())
    {
        [_pulseTimer invalidate];
        _pulseTimer = nil;
        return;
    }
    const bool still = NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
    const auto elapsed = std::chrono::milliseconds(static_cast<long long>((NSDate.timeIntervalSinceReferenceDate - _loadingSince) * 1000));
    const int active = still ? 0 : lunanx::LaunchPulseIndex(elapsed);
    for (int i = 0; i < 3; ++i) _pulse[i].alphaValue = i == active ? 1 : .23;
}

- (void)renderLibrary
{
    // Header: logo, name, clock and connection, account and settings.
    auto logo = [[LunaImageView alloc] initWithFrame:[self x:40 y:21 w:46 h:46]];
    logo.image = NSApp.applicationIconImage;
    [self paint:logo color:kShell radius:12 stroke:0 strokeColor:0];
    logo.layer.masksToBounds = YES;
    [_content addSubview:logo];
    [self put:[self label:@"LunaONE" size:25 color:kText bold:YES] in:_content frame:[self x:96 y:21 w:143 h:46]];
    _system = [self label:@"" size:15 color:kText bold:NO];
    _system.alignment = NSTextAlignmentCenter;
    [self shape:kShell radius:24 stroke:1 strokeColor:0x668e80a6 frame:[self x:_width - 638 y:20 w:372 h:48] in:_content];
    [self put:_system in:_content frame:[self x:_width - 630 y:20 w:356 h:48]];
    [self updateSystem];

    // Section icons, then account and settings: the top focus row.
    const bool searching = !_query.empty();
    __weak LunaCatalog* weak = self;
    // « Tous les jeux » holds the whole catalog, Luna Premium included.
    for (int i = 0; i < 3; ++i)
    {
        NSString* icon = @[@"trophy", @"clock", @"grid"][i];
        [self navigation:icon text:Label(kSections[i]) selected:(_section == i || (i == 2 && _section == 3)) && !searching
                       x:35 + i * 111 y:78 w:111 action:^{ [weak openSection:i]; } avatar:nil];
    }
    [self navigation:@"search" text:Label("Rechercher") selected:searching x:35 + 3 * 111 y:78 w:111
              action:^{ [weak showSearch]; } avatar:nil];
    [self navigation:@"account" text:Label("Compte") selected:NO x:_width - 241 y:7 w:86
              action:^{ [weak showAccount]; } avatar:Text(_profile.avatar_url)];
    [self navigation:@"settings" text:Label("Réglages") selected:NO x:_width - 133 y:7 w:86
              action:^{ [weak showSettings:-1]; } avatar:nil];

    // Heading: violet accent, section and count, status on the right.
    [self shape:kViolet radius:2 stroke:0 strokeColor:0 frame:[self x:64 y:165 w:4 h:20] in:_content];
    NSString* heading = searching ? Label("Rechercher") : Label(kSections[_section]);
    auto info = [[NSMutableAttributedString alloc] initWithString:heading
        attributes:@{NSFontAttributeName: [self font:20 bold:YES], NSForegroundColorAttributeName: Argb(kText)}];
    [info appendAttributedString:[[NSAttributedString alloc]
        initWithString:[@"   " stringByAppendingString:Text(lunanx::TrFormat("{} jeux", {std::to_string(_shown.size())}))]
            attributes:@{NSFontAttributeName: [self font:15 bold:NO], NSForegroundColorAttributeName: Argb(kSecondary)}]];
    auto headingLabel = [self label:@"" size:20 color:kText bold:YES];
    headingLabel.attributedStringValue = info;
    [self put:headingLabel in:_content frame:[self x:80 y:157 w:std::min<CGFloat>(510, _width / 2 - 64) h:36]];
    _status = [self label:_message size:14 color:kSecondary bold:NO];
    _status.alignment = NSTextAlignmentRight;
    [self put:_status in:_content frame:[self x:_width / 2 y:157 w:_width / 2 - 64 h:36]];

    [self renderCovers];
    [self renderDetails];

    // Footer: navigation hints for the device in use, version.
    _hints = [self label:@"" size:13 color:kSecondary bold:NO];
    [self put:_hints in:_content frame:[self x:64 y:682 w:std::max<CGFloat>(200, _width - 260) h:30]];
    auto version = [self label:Text(lunanx::VersionLabel()) size:13 color:kText bold:NO];
    version.alignment = NSTextAlignmentRight;
    [self put:version in:_content frame:[self x:_width - 165 y:682 w:130 h:30]];
    [self refreshSelection:YES];
    [self refreshFocus];
}

- (void)navigation:(NSString*)icon text:(NSString*)text selected:(BOOL)selected x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w
            action:(void (^)(void))action avatar:(NSString*)avatar
{
    const CGFloat diameter = 47;
    const int index = static_cast<int>(_top.size());
    __weak LunaCatalog* weak = self;
    auto box = [self control:[self x:x y:y w:w h:diameter + 25] in:_content clicked:^(NSInteger) {
        LunaCatalog* catalog = weak;
        if (!catalog || catalog->_busy) return;
        catalog->_zone = kTop;
        catalog->_topIndex = index;
        [catalog refreshFocus];
        action();
    }];
    auto disc = [[LunaFlippedView alloc] initWithFrame:[self local:(w - diameter) / 2 y:0 w:diameter h:diameter]];
    disc.wantsLayer = YES;
    disc.layer.masksToBounds = YES;
    [box addSubview:disc];
    auto glyph = [[LunaIcon alloc] initWithFrame:[self local:diameter / 4 y:diameter / 4 w:diameter / 2 h:diameter / 2]];
    glyph.type = icon;
    glyph.color = Argb(kText);
    [disc addSubview:glyph];
    if (avatar.length)
    {
        auto photo = [[LunaImageView alloc] initWithFrame:disc.bounds];
        [photo show:avatar];
        [disc addSubview:photo];
    }
    auto caption = [self label:text size:14 color:kText bold:YES];
    caption.alignment = NSTextAlignmentCenter;
    [self put:caption in:box frame:[self local:0 y:diameter + 3 w:w h:21]];
    __weak NSView* weakDisc = disc;
    __weak NSTextField* weakCaption = caption;
    box.decorate = ^(BOOL focused) {
        const bool marked = selected || focused;
        [weak paint:weakDisc color:focused ? 0xff3a2c66 : kShell radius:diameter / 2 stroke:marked ? 2 : 1
        strokeColor:marked ? kViolet : kRing];
        weakCaption.textColor = Argb(marked ? kViolet : kText);
    };
    box.decorate(NO);
    _top.push_back(box);
}

- (void)renderCovers
{
    const CGFloat start = 64, y = 211;
    _first = _selected / kPageSize * kPageSize;
    const std::size_t last = std::min(_first + kPageSize, _shown.size());
    const bool ranked = _section == 0 && _query.empty();
    const auto plan = _client->Plan();
    __weak LunaCatalog* weak = self;
    if (_simple)
    {
        const CGFloat width = std::min<CGFloat>(_width - 2 * start - 8, 590);
        auto clip = [[LunaFlippedView alloc] initWithFrame:[self x:start y:y w:width + 8 h:280]];
        clip.wantsLayer = YES;
        clip.layer.masksToBounds = YES;
        [_content addSubview:clip];
        _track = [[LunaFlippedView alloc] initWithFrame:[self local:0 y:0 w:width + 8 h:(last - _first) * 47 + 8]];
        [clip addSubview:_track];
        _trackLength = (last - _first) * 47 + 8;
        for (std::size_t i = _first; i < last; ++i)
        {
            const auto& game = _shown[i];
            auto row = [self control:[self local:4 y:4 + (i - _first) * 47 w:width h:42] in:_track
                             clicked:^(NSInteger clicks) { [weak coverClicked:i clicks:clicks]; }];
            [self paint:row color:kCard radius:17 stroke:1 strokeColor:kQuiet];
            row.layer.masksToBounds = YES;
            [self put:[self label:Text((ranked ? "#" + std::to_string(i + 1) + "  " : std::string()) + game.title)
                             size:18 color:kText bold:YES] in:row frame:[self local:12 y:0 w:width - 24 h:42]];
            _covers.push_back(row);
        }
    }
    else
    {
        const CGFloat card = 192, height = 256, gap = 22;
        _trackLength = start * 2 + (last - _first) * (card + gap);
        _track = [[LunaFlippedView alloc] initWithFrame:[self x:0 y:y w:std::max(_trackLength, _width) h:280]];
        [_content addSubview:_track];
        for (std::size_t i = _first; i < last; ++i)
        {
            const auto& game = _shown[i];
            const auto presentation = lunanx::ui::PresentLibraryGame(game, plan);
            auto cover = [self control:[self local:start + (i - _first) * (card + gap) y:12 w:card h:height] in:_track
                               clicked:^(NSInteger clicks) { [weak coverClicked:i clicks:clicks]; }];
            [self paint:cover color:kCard radius:17 stroke:1 strokeColor:kQuiet];
            cover.layer.masksToBounds = YES;
            auto placeholder = [self paragraph:Text(game.title) size:18 color:kText bold:YES in:cover
                                         frame:[self local:8 y:0 w:card - 16 h:height] centred:YES];
            placeholder.autoresizingMask = NSViewMinYMargin | NSViewMaxYMargin | NSViewWidthSizable;
            auto portrait = [[LunaImageView alloc] initWithFrame:cover.bounds];
            portrait.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [portrait show:Text(lunanx::ResizeAmazonImageUrl(presentation.cover, 480))];
            [cover addSubview:portrait];
            if (ranked)
            {
                auto badge = [self shape:kRank radius:13 stroke:0 strokeColor:0 frame:[self local:11 y:11 w:37 h:26] in:cover];
                auto rank = [self label:Text("#" + std::to_string(i + 1)) size:14 color:0xffffffff bold:YES];
                rank.alignment = NSTextAlignmentCenter;
                [self put:rank in:badge frame:badge.bounds];
            }
            if (presentation.locked)
            {
                auto holder = [self shape:kShell radius:12 stroke:0 strokeColor:0
                                    frame:[self local:card - 35 y:9 w:26 h:26] in:cover];
                holder.autoresizingMask = NSViewMinXMargin;
                auto lock = [[LunaIcon alloc] initWithFrame:holder.bounds];
                lock.type = @"lock";
                lock.color = Argb(kText);
                [holder addSubview:lock];
            }
            _covers.push_back(cover);
        }
    }
    if (_covers.empty())
        [self put:[self label:Label("Aucun jeu dans cette rubrique.") size:24 color:kSecondary bold:NO]
               in:_content frame:[self x:start y:y + 47 w:_width - start * 2 h:56]];
}

- (void)renderDetails
{
    const CGFloat left = 64, available = _width - left * 2;
    _title = [self label:@"" size:34 color:kText bold:YES];
    [self put:_title in:_content frame:[self x:left y:495 w:available h:54]];
    _offer = [self label:@"" size:15 color:kGreen bold:YES];
    [self put:_offer in:_content frame:[self x:left y:546 w:available h:32]];
    _metadata = [self label:@"" size:15 color:kSecondary bold:NO];
    [self put:_metadata in:_content frame:[self x:left y:546 w:available h:32]];

    __weak LunaCatalog* weak = self;
    _actions[kPlay] = [self pill:[@"▶   " stringByAppendingString:Label("JOUER")] primary:YES
                           frame:[self x:left y:594 w:220 h:52] action:^{ [weak focusAction:kPlay]; [weak playSelected]; }];
    _actions[kCancel] = [self pill:Label("Annuler") primary:NO frame:[self x:_width - left - 134 y:594 w:134 h:52]
                            action:^{ [weak cancelJob]; }];
    const std::size_t count = _shown.size();
    if (count > kPageSize)
    {
        const std::size_t pages = (count + kPageSize - 1) / kPageSize, page = _first / kPageSize;
        const CGFloat x = left + 244;
        _actions[kPrevious] = [self pill:@"‹" primary:NO frame:[self x:x y:598 w:42 h:44]
                                  action:^{ [weak focusAction:kPrevious]; [weak turnPage:-1]; }];
        _actions[kPrevious].hidden = page == 0;
        auto index = [self label:Text(std::to_string(page + 1) + " / " + std::to_string(pages)) size:15 color:kSecondary bold:YES];
        index.alignment = NSTextAlignmentCenter;
        [self put:index in:_content frame:[self x:x + 46 y:598 w:62 h:44]];
        _actions[kNext] = [self pill:@"›" primary:NO frame:[self x:x + 112 y:598 w:42 h:44]
                              action:^{ [weak focusAction:kNext]; [weak turnPage:1]; }];
        _actions[kNext].hidden = page + 1 >= pages;
    }
    [self refreshBusy];
}

- (void)refreshBusy
{
    for (LunaControl* control : _top) control.disabled = _busy;
    for (LunaControl* control : _covers) control.disabled = _busy;
    for (LunaControl* control : _loginControls) control.disabled = _busy && control != _loginControls.back();
    for (int i = kPlay; i <= kNext; ++i) _actions[i].disabled = _busy;
    _actions[kCancel].hidden = !_busy;
    _actions[kCancel].disabled = _busy && _cancelled.load();
    if (_busy && _client) { _zone = kActions; _actionIndex = kCancel; }
    else if (_zone == kActions && _actionIndex == kCancel) _zone = kCovers;
    if (_busy && !_client && !_loginControls.empty()) _loginFocus = _loginControls.size() - 1;
}

- (void)updateSystem
{
    if (!_system) return;
    auto formatter = [NSDateFormatter new];
    formatter.timeStyle = NSDateFormatterShortStyle;
    formatter.dateStyle = NSDateFormatterNoStyle;
    std::string summary = [formatter stringFromDate:[NSDate date]].UTF8String;
    if (const auto network = _network->Label(); !network.empty()) summary += " · " + network;
    if (const int battery = lunaone::apple::SystemBatteryPercent(); battery >= 0) summary += " · " + std::to_string(battery) + "%";
    if (_client)
    {
        const auto plan = _client->Plan();
        const auto label = plan == AccountPlan::Premium ? std::string("Luna Premium") :
                           plan == AccountPlan::Standard ? std::string("Luna Standard") : _client->PlanLabel();
        if (!label.empty()) summary += "   │   " + label;
    }
    _system.stringValue = Text(summary);
}

#pragma mark - Library selection

- (void)refreshSelection:(BOOL)immediate
{
    if (_covers.empty())
    {
        _actions[kPlay].hidden = YES;
        _title.stringValue = _offer.stringValue = _metadata.stringValue = @"";
        return;
    }
    const auto& game = _shown.at(_selected);
    const auto presentation = lunanx::ui::PresentLibraryGame(game, _client->Plan());
    _title.stringValue = Text(game.title);
    _offer.stringValue = Text(presentation.offer);
    _offer.textColor = Argb(presentation.included ? kGreen : kViolet);
    [_offer sizeToFit];
    const CGFloat shift = presentation.offer.empty() ? 0 : _offer.frame.size.width + 14 * _scale;
    _metadata.stringValue = Text(presentation.metadata);
    _metadata.frame = NSMakeRect(64 * _scale + shift, _offer.frame.origin.y, (_width - 128) * _scale - shift,
                                 _metadata.frame.size.height);
    for (std::size_t i = 0; i < _covers.size(); ++i)
    {
        LunaControl* cover = _covers[i];
        const bool chosen = _first + i == _selected;
        cover.layer.borderWidth = (chosen ? 3 : 1) * _scale;
        cover.layer.borderColor = Argb(chosen ? kViolet : kQuiet).CGColor;
        if (_simple) continue;
        // Selected cover grows by 5.5 % around its centre, as on the TV.
        const NSRect base = [self local:64 + i * 214 y:12 w:192 h:256];
        const CGFloat grow = chosen ? .0275 : 0;
        const NSRect frame = NSInsetRect(base, -base.size.width * grow, -base.size.height * grow);
        if (immediate) cover.frame = frame;
        else
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
                context.duration = .16;
                cover.animator.frame = frame;
            }];
    }
    [self scrollToSelection:immediate];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(loadBackdrop) object:nil];
    [self performSelector:@selector(loadBackdrop) withObject:nil afterDelay:immediate ? 0 : .13];
}

- (void)scrollToSelection:(BOOL)immediate
{
    if (!_track || _covers.empty()) return;
    // In points: an unsigned index would wrap below zero.
    const CGFloat index = _selected - _first;
    if (_simple)
    {
        const CGFloat visible = 280, top = 4 + index * 47;
        CGFloat offset = _trackOffset;
        if (top < offset) offset = top - 4;
        if (top + 46 > offset + visible) offset = top + 46 - visible;
        [self setTrackOffset:offset animated:!immediate];
    }
    else
        [self setTrackOffset:64 + index * 214 - 104 animated:!immediate];
}

- (void)setTrackOffset:(CGFloat)offset animated:(BOOL)animated
{
    const CGFloat visible = _simple ? 280 : _width;
    _trackOffset = std::clamp<CGFloat>(offset, 0, std::max<CGFloat>(0, _trackLength - visible));
    NSPoint origin = _track.frame.origin;
    if (_simple) origin.y = -_trackOffset * _scale;
    else origin.x = -_trackOffset * _scale;
    if (!animated || NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion) _track.frameOrigin = origin;
    else
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
            context.duration = .25;
            context.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
            self->_track.animator.frameOrigin = origin;
        }];
}

- (void)loadBackdrop
{
    if (!_client || _shown.empty()) return;
    const auto presentation = lunanx::ui::PresentLibraryGame(_shown.at(_selected), _client->Plan());
    [_backdrop show:Text(lunanx::ResizeAmazonImageUrl(presentation.backdrop, 1280))];
}

- (void)select:(std::size_t)index
{
    if (index >= _shown.size() || _busy) return;
    const bool samePage = index / kPageSize == _first / kPageSize;
    _selected = index;
    if (samePage) [self refreshSelection:NO];
    else [self render];
}

- (void)coverClicked:(std::size_t)index clicks:(NSInteger)clicks
{
    if (_busy) return;
    _zone = kCovers;
    // As on a touch screen: the first click selects, the next one plays.
    if (index == _selected || clicks >= 2)
    {
        [self select:index];
        [self playSelected];
        return;
    }
    [self select:index];
    [self refreshFocus];
}

- (void)turnPage:(int)direction
{
    const auto next = direction > 0 ? _first + kPageSize : _first == 0 ? _shown.size() : _first - 1;
    if (next >= _shown.size() || _busy) return;
    _selected = next;
    [self render];
}

- (void)focusAction:(Action)action
{
    _zone = kActions;
    _actionIndex = action;
    [self refreshFocus];
}

- (void)refreshFocus
{
    const bool keys = _device != Device::Mouse;
    if (_client)
    {
        for (std::size_t i = 0; i < _top.size(); ++i)
            _top[i].decorate(keys && _zone == kTop && static_cast<int>(i) == _topIndex);
        for (int i = kPlay; i <= kCancel; ++i)
            if (_actions[i]) _actions[i].decorate(keys && _zone == kActions && _actionIndex == i);
    }
    else
        for (std::size_t i = 0; i < _loginControls.size(); ++i)
            _loginControls[i].decorate(keys && i == _loginFocus);
    [self refreshHints];
}

- (void)refreshHints
{
    if (!_hints) return;
    const bool swapped = lunanx::LoadPlaybackPreferences().swap_ab;
    // In full screen, Esc goes back to the window.
    const std::string fullscreen = lunaone::apple::IsBorderlessFullscreen(_window)
        ? "Esc  " + lunanx::Tr("Fenêtre") : "⌃⌘F  " + lunanx::Tr("Plein écran");
    std::string text;
    switch (_device)
    {
    case Device::Gamepad:
    {
        // Named as printed on the controller in hand: A or ✕, LB or L1…
        using lunanx::input::PadButton;
        GCController* controller = lunaone::apple::ActiveGamepad();
        const auto family = lunaone::apple::ControllerFamilyOf(controller);
        const auto name = [&](PadButton button) { return lunaone::apple::ButtonLabel(controller, button); };
        text = name(swapped ? PadButton::B : PadButton::A) + "  " + lunanx::Tr("Valider") + "    " +
               name(swapped ? PadButton::A : PadButton::B) + "  " + lunanx::Tr("Retour") + "    " +
               name(PadButton::Menu) + "  " + lunanx::Tr("Menu") +
               (lunanx::LoadPlaybackPreferences().virtual_keyboard
                    ? "    " + lunanx::input::KeyboardChordName(family) + "  " + lunanx::Tr("Clavier") : "") +
               "    " + lunanx::TrFormat("Quitter une partie : {}", {lunanx::input::ExitChordName(family)});
        break;
    }
    case Device::Keyboard:
        text = "← → ↑ ↓ " + lunanx::Tr("Naviguer") + "    ↩ " + lunanx::Tr("Valider") + "    ⌫ " + lunanx::Tr("Retour") +
               "    " + fullscreen;
        break;
    case Device::Mouse:
        text = _client ? lunanx::Tr("Double-clic : jouer") + "    " + fullscreen : fullscreen;
        break;
    }
    _hints.stringValue = Text(text);
}

- (void)usedDevice:(Device)device
{
    if (_device == device) return;
    _device = device;
    [self refreshFocus];
}

#pragma mark - Sign-in screen

// The sign-in screen's backdrop: the logo's violet deepening to the
// library's night blue, with two soft glows.
- (void)addLoginBackdrop
{
    NSView* backdrop = [[LunaFlippedView alloc] initWithFrame:_content.bounds];
    backdrop.wantsLayer = YES;
    CAGradientLayer* base = [CAGradientLayer layer];
    base.frame = backdrop.bounds;
    base.colors = @[(__bridge id)Argb(0xff3a1f9a).CGColor, (__bridge id)Argb(0xff1b1150).CGColor,
                    (__bridge id)Argb(0xff0c0820).CGColor];
    base.locations = @[@0, @.55, @1];
    base.startPoint = CGPointMake(0, 0);
    base.endPoint = CGPointMake(1, 1);
    [backdrop.layer addSublayer:base];
    const CGFloat aspect = backdrop.bounds.size.width / std::max<CGFloat>(1, backdrop.bounds.size.height);
    const struct { CGPoint centre; CGFloat radius; std::uint32_t colour; } glows[] = {
        {CGPointMake(.82, .18), .45, 0x707c4dff}, {CGPointMake(.08, .92), .4, 0x40b06bff}};
    for (const auto& glow : glows)
    {
        CAGradientLayer* light = [CAGradientLayer layer];
        light.type = kCAGradientLayerRadial;
        light.frame = backdrop.bounds;
        light.colors = @[(__bridge id)Argb(glow.colour).CGColor, (__bridge id)Argb(glow.colour & 0x00ffffff).CGColor];
        light.startPoint = glow.centre;
        light.endPoint = CGPointMake(glow.centre.x + glow.radius, glow.centre.y + glow.radius * aspect);
        [backdrop.layer addSublayer:light];
    }
    [_content addSubview:backdrop];
}

// The form on the left and, on the right, the steps of the sign-in or the
// code with its QR. The 1280 × 720 design scales to fit the window in both
// directions and stays centred, so that it never leaves a TV's picture.
- (void)renderLogin
{
    _offsetX = std::max<CGFloat>(0, (_root.bounds.size.width - 1280 * _scale) / 2);
    [self addLoginBackdrop];

    auto logo = [[LunaImageView alloc] initWithFrame:[self x:88 y:56 w:72 h:72]];
    logo.image = NSApp.applicationIconImage;
    logo.layer.cornerRadius = 16 * _scale;
    [_content addSubview:logo];
    [self put:[self label:@"LunaONE" size:40 color:kText bold:YES] in:_content frame:[self x:176 y:56 w:440 h:46]];
    [self put:[self label:Label("Connexion Amazon") size:18 color:kViolet bold:NO] in:_content
        frame:[self x:178 y:102 w:440 h:26]];
    [self paragraph:Label("Tes jeux Amazon Luna, sans navigateur, sur tous tes écrans.") size:17 color:kSecondary
               bold:NO in:_content frame:[self x:88 y:148 w:480 h:48] centred:NO];

    // The form, on a frosted card.
    __weak LunaCatalog* weak = self;
    constexpr CGFloat kCardTop = 212, kFormX = 112, kFormWidth = 432;
    const CGFloat cardHeight = _saved ? 22 + 3 * 56 + 2 * 12 + 22 : 22 + 34 + 3 * 56 + 2 * 12 + 22;
    [self shape:kLoginCard radius:18 stroke:1 strokeColor:kLoginCardEdge frame:[self x:88 y:kCardTop w:480 h:cardHeight]
             in:_content];
    CGFloat y = kCardTop + 22;
    if (_saved)
    {
        [self button:Label("Réessayer le chargement") primary:YES y:y action:^{ [weak connect]; } x:kFormX w:kFormWidth];
        [self button:Label("Se déconnecter") primary:NO y:y += 68 action:^{ [weak confirmSignOut]; } x:kFormX w:kFormWidth];
    }
    else
    {
        [self put:[self label:Label("Pays du compte") size:16 color:kSecondary bold:NO] in:_content
            frame:[self x:kFormX y:y w:kFormWidth h:26]];
        y += 34;
        std::string market = _market;
        for (const auto& entry : AmazonMarkets())
            if (entry.code == _market) market = entry.native_name;
        [self button:[Text(market) stringByAppendingString:@"   ▾"] primary:NO y:y action:^{ [weak showMarkets]; }
                   x:kFormX w:kFormWidth];
        [self button:Label("Créer un code") primary:YES y:y += 68 action:^{ [weak connect]; } x:kFormX w:kFormWidth];
    }
    [self button:Label("Réglages") primary:NO y:y += 68 action:^{ [weak showSettings:-1]; } x:kFormX w:kFormWidth];

    // The right card.
    auto panel = [self shape:kLoginCard radius:18 stroke:1 strokeColor:kLoginCardEdge frame:[self x:640 y:112 w:552 h:456]
                          in:_content];
    if (_code)
    {
        [self paragraph:Label("Scanne le QR code et saisis le code Amazon.") size:18 color:kText bold:YES in:panel
                  frame:[self local:28 y:24 w:496 h:50] centred:YES];
        // The QR keeps a white plate: phone cameras read it best that way.
        auto plate = [self shape:0xffffffff radius:16 stroke:0 strokeColor:0
                           frame:[self local:(552 - 212) / 2.0 y:84 w:212 h:212] in:panel];
        auto qr = [[LunaImageView alloc] initWithFrame:[self local:16 y:16 w:180 h:180]];
        qr.layer.contentsGravity = kCAGravityResizeAspect;
        qr.layer.magnificationFilter = kCAFilterNearest;
        qr.image = _qr;
        [plate addSubview:qr];
        auto address = [self label:_codeAddress size:19 color:kViolet bold:NO];
        address.alignment = NSTextAlignmentCenter;
        [self put:address in:panel frame:[self local:28 y:314 w:496 h:30]];
        auto code = [self label:_code size:50 color:kText bold:YES];
        code.font = [NSFont monospacedSystemFontOfSize:50 * _scale weight:NSFontWeightBold];
        code.alignment = NSTextAlignmentCenter;
        [self put:code in:panel frame:[self local:28 y:350 w:496 h:72]];
    }
    else
    {
        [self put:[self label:Label("Comment se connecter") size:22 color:kText bold:YES] in:panel
            frame:[self local:32 y:28 w:488 h:32]];
        const char* steps[] = {"Choisis le pays de ton compte Amazon, puis crée un code.",
                               "Scanne le QR code avec ton téléphone ou ouvre la page Amazon.",
                               "Saisis le code et confirme : ta bibliothèque Luna s’ouvre ici."};
        for (int i = 0; i < 3; ++i)
        {
            const CGFloat top = 88 + i * 104;
            auto badge = [self shape:kLoginButton radius:22 stroke:0 strokeColor:0 frame:[self local:32 y:top w:44 h:44]
                                  in:panel];
            auto number = [self label:[NSString stringWithFormat:@"%d", i + 1] size:20 color:kText bold:YES];
            number.alignment = NSTextAlignmentCenter;
            [self put:number in:badge frame:badge.bounds];
            [self paragraph:Label(steps[i]) size:17 color:kSecondary bold:NO in:panel
                      frame:[self local:96 y:top w:424 h:76] centred:NO];
        }
        [self paragraph:Label("Aucun mot de passe n’est saisi dans LunaONE.") size:15 color:kViolet bold:NO in:panel
                  frame:[self local:32 y:404 w:488 h:30] centred:NO];
    }

    y = kCardTop + cardHeight;
    _status = [self label:_message size:16 color:kSecondary bold:NO];
    [self put:_status in:_content frame:[self x:88 y:y + 12 w:480 h:28]];
    // The cancel button is always the last focusable control.
    auto cancel = [self button:Label("Annuler") primary:NO y:y + 48 action:^{ [weak cancelJob]; } x:88 w:480];
    cancel.hidden = !_busy;
    _hints = [self label:@"" size:13 color:kSecondary bold:NO];
    [self put:_hints in:_content frame:[self x:88 y:682 w:900 h:30]];
    if (_loginFocus >= _loginControls.size()) _loginFocus = 0;
    [self refreshBusy];
    [self refreshFocus];
    // The settings and panels drawn next use the full width.
    _offsetX = 0;
}

// Sign-in buttons: primary in violet, secondary frosted, a white or violet ring on focus.
- (LunaControl*)button:(NSString*)title primary:(BOOL)primary y:(CGFloat)y action:(void (^)(void))action
{
    return [self button:title primary:primary y:y action:action x:88 w:480];
}

- (LunaControl*)button:(NSString*)title primary:(BOOL)primary y:(CGFloat)y action:(void (^)(void))action
                     x:(CGFloat)x w:(CGFloat)w
{
    const std::size_t index = _loginControls.size();
    __weak LunaCatalog* weak = self;
    auto button = [self control:[self x:x y:y w:w h:56] in:_content clicked:^(NSInteger) {
        LunaCatalog* catalog = weak;
        if (!catalog) return;
        catalog->_loginFocus = index;
        [catalog refreshFocus];
        action();
    }];
    auto label = [self label:title size:19 color:kText bold:YES];
    label.alignment = NSTextAlignmentCenter;
    [self put:label in:button frame:button.bounds];
    __weak LunaControl* weakButton = button;
    button.decorate = ^(BOOL focused) {
        [weak paint:weakButton
                 color:primary ? (focused ? kLoginButtonFocus : kLoginButton) : (focused ? kLoginSecondaryFocus : kLoginSecondary)
                radius:12 stroke:focused ? 3 : (primary ? 0 : 1)
           strokeColor:focused ? (primary ? 0xffffffff : kViolet) : kLoginOutline];
    };
    button.decorate(NO);
    _loginControls.push_back(button);
    return button;
}

- (void)showMarkets
{
    NSMutableArray* rows = [NSMutableArray new];
    for (const auto& market : AmazonMarkets())
    {
        const std::string code = market.code;
        auto row = Row(Text(market.native_name), nil, ^{
            self->_market = code;
            [self closePanel];
        });
        row.checked = code == _market;
        [rows addObject:row];
    }
    [self openPanel:Label("Pays du compte") rows:rows back:^{ [self closePanel]; }];
}

#pragma mark - Jobs

- (void)connect
{
    if (_busy) return;
    _busy = true;
    _cancelled = false;
    const bool saved = _saved;
    NSString* market = Text(_market);
    _message = Label(saved ? "Chargement du catalogue Luna…" : "Création du code Amazon…");
    _loading = saved;
    _loadingSince = NSDate.timeIntervalSinceReferenceDate;
    [self closePanel];
    [self render];
    [_jobs addOperationWithBlock:^{
        try
        {
            DeviceRegistration registration;
            if (!saved)
            {
                registration = AmazonAuth::SignInWithCode(market.UTF8String, [self](const auto& code) {
                    NSString* address = Text(code.verification_url);
                    NSString* token = Text(code.user_code);
                    NSImage* qr = QrImage(code.verification_url);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        self->_codeAddress = address;
                        self->_code = token;
                        self->_qr = qr;
                        self->_message = Label("Confirme ce code sur Amazon, puis reviens dans LunaONE.");
                        [self render];
                    });
                }, [self] { return self->_cancelled.load(); });
                if (self->_cancelled || !AmazonAuth::Save(registration))
                {
                    try { AmazonAuth::Deregister(registration); } catch (const std::exception&) {}
                    throw std::runtime_error("Could not save sign-in.");
                }
                dispatch_async(dispatch_get_main_queue(), ^{ self->_saved = true; });
            }
            else if (!AmazonAuth::Load(registration))
                throw std::runtime_error("Could not read the saved Amazon sign-in.");
            lunanx::SetUiLanguageForMarket(registration.market_code);
            CookieJar jar;
            jar.SetCookies(registration.cookies);
            auto client = std::make_shared<LunaClient>(std::move(jar), std::move(registration));
            client->SignIn();
            auto shelves = client->FetchShelves();
            std::vector<Game> games;
            std::set<std::string> keys;
            for (const auto& shelf : shelves)
                for (const auto& game : shelf.games)
                    if (InsertGameKeys(keys, game)) games.push_back(game);
            auto profile = client->Profile();
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_busy = false;
                self->_loading = false;
                self->_saved = true;
                self->_code = nil;
                self->_client = client;
                self->_shelves = shelves;
                self->_catalog = games;
                self->_profile = profile;
                self->_message = @"";
                self->_zone = kCovers;
                [self openSection:2];
                [self exploreQuietly];
            });
        }
        catch (const std::exception& error)
        {
            NSString* message = self->_cancelled ? Label("Opération annulée.") : Text(error.what());
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_busy = false;
                self->_loading = false;
                self->_code = nil;
                self->_message = message;
                [self render];
            });
        }
    }];
}

- (void)cancelJob
{
    if (!_busy) return;
    _cancelled = true;
    _message = Label("Annulation…");
    [self render];
}

- (void)openSection:(int)section
{
    if (_busy || !_client) return;
    const auto sections = std::array{LibrarySection::Top, LibrarySection::Recent, LibrarySection::All, LibrarySection::Premium};
    _section = section;
    _query.clear();
    _shown = lunanx::ui::LibraryGames(sections.at(section), _shelves, _catalog, _client->Plan());
    _selected = 0;
    _trackOffset = 0;
    [self render];
}

// The query is taken by value: the blocks below outlive the caller's string.
- (void)search:(std::string)query
{
    if (_busy || !_client || query.empty()) return;
    _busy = true;
    _cancelled = false;
    _message = Label("Recherche dans le catalogue Luna…");
    [self render];
    auto client = _client;
    [_jobs addOperationWithBlock:^{
        std::vector<Game> found;
        NSString* error = nil;
        try { found = client->SearchGames(query, [self] { return !self->_cancelled.load(); }); }
        catch (const std::exception& failure) { error = Text(failure.what()); }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy = false;
            if (self->_cancelled) self->_message = Label("Opération annulée.");
            else if (error) self->_message = error;
            else
            {
                self->_shown = found;
                self->_query = query;
                self->_selected = 0;
                self->_trackOffset = 0;
                self->_zone = kCovers;
                self->_message = @"";
            }
            [self render];
        });
    }];
}

- (void)explore
{
    if (_busy || !_client) return;
    _busy = true;
    _cancelled = false;
    _message = Label("Exploration du catalogue Luna…");
    [self render];
    auto client = _client;
    std::set<std::string> known;
    for (const auto& game : _catalog) InsertGameKeys(known, game);
    const std::size_t base = _catalog.size();
    [_jobs addOperationWithBlock:^{
        auto found = std::make_shared<std::vector<Game>>();
        NSString* error = nil;
        try
        {
            client->ExploreCatalog(known, [&](std::vector<Game> batch) {
                for (auto& game : batch) found->push_back(std::move(game));
                NSString* progress = Text(lunanx::TrFormat("{} titres trouvés…", {std::to_string(base + found->size())}));
                dispatch_async(dispatch_get_main_queue(), ^{
                    self->_message = progress;
                    self->_status.stringValue = progress;
                });
            }, [self] { return !self->_cancelled.load(); });
        }
        catch (const std::exception& failure) { error = Text(failure.what()); }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy = false;
            for (auto& game : *found) self->_catalog.push_back(std::move(game));
            self->_message = error ?: Text(lunanx::TrFormat("{} titres", {std::to_string(self->_catalog.size())}));
            [self openSection:2];
        });
    }];
}

// The home page lists only part of Luna: the linked pages (« Tout voir »,
// genres, collections) are explored quietly after loading, and « Tous les
// jeux » grows as games arrive. A game stops the exploration until it ends.
- (void)exploreQuietly
{
    if (_exploring) _exploring->store(false);
    if (!_client) return;
    auto client = _client;
    auto running = std::make_shared<std::atomic_bool>(true);
    _exploring = running;
    std::set<std::string> known;
    for (const auto& game : _catalog) InsertGameKeys(known, game);
    std::thread([self, client, running, known]() mutable {
        try
        {
            client->ExploreCatalog(std::move(known), [&](std::vector<Game> batch) {
                dispatch_async(dispatch_get_main_queue(), ^{ [self addExplored:batch client:client]; });
            }, [running] { return running->load(); });
        }
        catch (const std::exception&) {}
    }).detach();
}

- (void)addExplored:(std::vector<Game>)games client:(std::shared_ptr<LunaClient>)client
{
    if (client != _client || games.empty()) return;
    std::set<std::string> known;
    for (const auto& game : _catalog) InsertGameKeys(known, game);
    for (auto& game : games)
        if (InsertGameKeys(known, game)) _catalog.push_back(std::move(game));
    if (_section != 2 || !_query.empty() || _busy) return;
    // The selected game stays selected while the list grows.
    const std::string asin = _shown.empty() ? std::string() : _shown.at(_selected).asin;
    _shown = lunanx::ui::LibraryGames(LibrarySection::All, _shelves, _catalog, _client->Plan());
    for (std::size_t i = 0; i < _shown.size(); ++i)
        if (!asin.empty() && _shown[i].asin == asin) _selected = i;
    if (_settings || _panelRows) return;
    [self render];
}

- (void)playSelected
{
    if (_busy || !_client || _shown.empty()) return;
    const auto game = _shown.at(_selected);
    if (lunanx::ui::PresentLibraryGame(game, _client->Plan()).locked)
    {
        _message = Label("Nécessite Luna Premium");
        _status.stringValue = _message;
        return;
    }
    [self closePanel];
    // The game gets the whole connection.
    const bool exploring = _exploring && _exploring->load();
    if (_exploring) _exploring->store(false);
    NSString* result = nil;
    try { lunaone::apple::Play(*_client, game, {}, [] { return false; }, true); }
    catch (const std::exception& error) { result = Text(error.what()); }
    // Buttons still held from the exit chord must not act on the catalog.
    _padLatched = true;
    _message = result ?: Label("Partie fermée.");
    if (exploring) [self exploreQuietly];
    lunaone::apple::SetBorderlessFullscreen(_window, lunanx::LoadPlaybackPreferences().fullscreen);
    lunaone::apple::UpdatePresentation();
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_root];
    if (_section == 1 && _query.empty())
    {
        const auto selected = _selected;
        [self openSection:1];
        _selected = std::min(selected, _shown.empty() ? 0 : _shown.size() - 1);
    }
    [self render];
}

- (void)confirmSignOut
{
    [self openPanel:Label("Déconnecter ce compte de LunaONE ?") rows:@[
        Button(Label("Annuler"), ^{ [self closePanel]; }),
        Button(Label("Se déconnecter"), ^{ [self closePanel]; [self signOut]; })] back:^{ [self closePanel]; }];
}

- (void)signOut
{
    if (_busy) return;
    _busy = true;
    _message = Label("Déconnexion…");
    [self render];
    [_jobs addOperationWithBlock:^{
        DeviceRegistration registration;
        if (AmazonAuth::Load(registration))
            try { AmazonAuth::Deregister(registration); } catch (const std::exception&) {}
        const bool erased = AmazonAuth::EraseAccountData();
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy = false;
            if (!erased)
            {
                self->_message = Label("Impossible d’effacer la connexion enregistrée.");
                [self render];
                return;
            }
            lunanx::SetUiLanguageForMarket(lunanx::LoadPlaybackPreferences().market);
            self->_client.reset();
            self->_catalog.clear();
            self->_shown.clear();
            self->_shelves.clear();
            self->_profile = {};
            self->_saved = false;
            self->_settings = NO;
            self->_message = @"";
            [self->_backdrop show:@""];
            [self render];
        });
    }];
}

- (void)checkUpdates
{
    if (_checkingUpdates) return;
    _checkingUpdates = YES;
    [self openPanel:Label("Vérifier les mises à jour") rows:@[Note(Label("Vérification des mises à jour…"), 3)]
               back:^{ [self closePanel]; }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const lunanx::UpdateInfo update = lunanx::FindUpdate();
        const BOOL installable = lunanx::CanInstallUpdate(update);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_checkingUpdates = NO;
            NSMutableArray* rows = [NSMutableArray arrayWithObject:Note(Text(update.message), 3)];
            if (installable)
                [rows addObject:Button(Text(lunanx::TrFormat("Installer la version {}", {"V" + update.version})), ^{
                    [self installUpdate:update];
                })];
            else if (update.available)
                [rows addObject:Note(Label("Télécharge la nouvelle version sur GitHub."), 3)];
            [rows addObject:Button(Label("Versions GitHub"), ^{
                [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:Text(lunanx::ReleasesUrl)]];
            })];
            [rows addObject:Button(@"OK", ^{ [self closePanel]; })];
            [self openPanel:Label("Vérifier les mises à jour") rows:rows back:^{ [self closePanel]; }];
        });
    });
}

// Downloads and installs the update, then quits: the new version opens once
// this one has ended.
- (void)installUpdate:(const lunanx::UpdateInfo&)update
{
    if (_checkingUpdates) return;
    _checkingUpdates = YES;
    [self openPanel:Label("Mise à jour de LunaONE") rows:@[
        Note(Label("Téléchargement de la mise à jour…"), 3),
        Note(Label("LunaONE redémarre tout seul une fois la mise à jour installée."), 3)] back:^{}];
    const lunanx::UpdateInfo copy = update;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* error = nil;
        try { lunanx::InstallUpdate(copy); }
        catch (const std::exception& failure) { error = Text(failure.what()); }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_checkingUpdates = NO;
            if (!error)
            {
                [NSApp terminate:nil];
                return;
            }
            [self openPanel:Label("Mise à jour impossible") rows:@[
                Note(error, 3),
                Button(Label("Versions GitHub"), ^{
                    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:Text(lunanx::ReleasesUrl)]];
                }),
                Button(@"OK", ^{ [self closePanel]; })] back:^{ [self closePanel]; }];
        });
    });
}

#pragma mark - Panels

- (void)showAccount
{
    if (_busy || !_client) return;
    NSMutableArray* rows = [NSMutableArray new];
    if (!_profile.name.empty()) [rows addObject:Note(Text(_profile.name), 2)];
    if (const auto plan = _client->PlanLabel(); !plan.empty()) [rows addObject:Note(Text(plan), 2)];
    [rows addObject:Button(Label("Annuler"), ^{ [self closePanel]; })];
    [rows addObject:Button(Label("Se déconnecter"), ^{ [self confirmSignOut]; })];
    [self openPanel:Label("Compte") rows:rows back:^{ [self closePanel]; }];
}

- (void)showSearch
{
    if (_busy || !_client) return;
    _panelField = [NSTextField textFieldWithString:Text(_query)];
    _panelField.delegate = self;
    _panelField.target = self;
    _panelField.action = @selector(submitSearch:);
    // Only Return searches: rebuilding the panel ends the editing session,
    // which must not start a search after every on-screen key.
    _panelField.cell.sendsActionOnEndEditing = NO;
    const bool keyboard = lunanx::LoadPlaybackPreferences().virtual_keyboard;
    NSMutableArray* buttons = [NSMutableArray arrayWithObject:Button(Label("Annuler"), ^{ [self closePanel]; })];
    if (keyboard)
        [buttons addObject:Button(Label("Clavier"), ^{
            if (self->_keyboard) self->_keyboard.reset();
            else self->_keyboard = std::make_unique<lunanx::ui::VirtualKeyboard>(lunanx::UiLanguage());
            [self renderPanel];
        })];
    [buttons addObject:Button(Label("Rechercher"), ^{ [self submitSearch:nil]; })];
    [self openPanel:Label("Rechercher un jeu") rows:buttons back:^{ [self closePanel]; }];
    [self editField];
    // With a controller in hand, the search opens with the keyboard.
    if (keyboard && _device == Device::Gamepad)
    {
        _keyboard = std::make_unique<lunanx::ui::VirtualKeyboard>(lunanx::UiLanguage());
        [self renderPanel];
    }
}

// Puts the typing focus in the search field, caret at the end, in white.
- (void)editField
{
    if (!_panelField) return;
    [_window makeFirstResponder:_panelField];
    auto editor = (NSTextView*)_panelField.currentEditor;
    editor.insertionPointColor = NSColor.whiteColor;
    [editor moveToEndOfDocument:nil];
}

- (void)submitSearch:(id)sender
{
    const std::string query = [_panelField.stringValue stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet].UTF8String ?: "";
    if (query.empty())
    {
        [self editField];
        return;
    }
    [self closePanel];
    [self search:query];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command
{
    if (command == @selector(cancelOperation:)) { [self closePanel]; return YES; }
    if (command == @selector(moveDown:))
    {
        // Down leaves the field for the buttons, the main one first.
        [_window makeFirstResponder:_root];
        _device = Device::Keyboard;
        _panelFocus = static_cast<NSInteger>(_panelRows.count) - 1;
        [self renderPanel];
        return YES;
    }
    return NO;
}

#pragma mark - Settings screen

// The settings screen: categories on the left, the rows of the chosen one on
// the right. A row with two values is a switch; another opens its values in
// the right pane. ← or Back returns one level, up to the categories.
- (std::vector<SettingsCategory>)settingsCategories
{
    const auto choices = lunanx::PlaybackChoices(lunanx::LoadPlaybackPreferences(), true, true, true);
    std::vector<SettingsCategory> categories;
    const auto section = [&](lunanx::PlaybackSection which, NSString* icon) {
        SettingsCategory category {Label(lunanx::PlaybackSectionTitle(which)), icon, {}};
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            const auto& choice = choices[i];
            if (choice.section != which) continue;
            SettingsEntry entry;
            entry.title = Text(choice.title);
            entry.value = Text(choice.labels.at(choice.selected));
            entry.choice = static_cast<int>(i);
            // Only a no/yes or off/on choice is a switch; other pairs list their values.
            const bool binary = choice.labels.size() == 2 &&
                ((choice.labels[0] == lunanx::Tr("Non") && choice.labels[1] == lunanx::Tr("Oui")) ||
                 (choice.labels[0] == lunanx::Tr("Désactivé") && choice.labels[1] == lunanx::Tr("Activé")));
            entry.kind = binary ? 1 : 0;
            entry.on = choice.selected == 1;
            category.rows.push_back(entry);
        }
        return category;
    };
    categories.push_back(section(lunanx::PlaybackSection::General, @"settings"));
    auto controller = section(lunanx::PlaybackSection::Controller, @"gamepad");
    // The shortcuts, named for the controller in hand.
    const auto family = lunaone::apple::ControllerFamilyOf(lunaone::apple::ActiveGamepad());
    for (const auto& note : {lunanx::TrFormat("Quitter une partie : {}", {lunanx::input::ExitChordName(family)}),
                             lunanx::TrFormat("Clavier virtuel : {}", {lunanx::input::KeyboardChordName(family)}),
                             lunanx::TrFormat("Infos techniques en jeu : {}", {lunanx::input::OverlayChordName(family)})})
    {
        SettingsEntry entry;
        entry.kind = 3;
        entry.title = Text(note);
        controller.rows.push_back(entry);
    }
    categories.push_back(controller);
    categories.push_back(section(lunanx::PlaybackSection::Advanced, @"display"));
    if (_saved)
    {
        SettingsCategory account {Label("Compte"), @"account", {}};
        for (const auto& line : {_profile.name, _client ? _client->PlanLabel() : std::string()})
            if (!line.empty())
            {
                SettingsEntry entry;
                entry.kind = 3;
                entry.title = Text(line);
                account.rows.push_back(entry);
            }
        SettingsEntry signOut;
        signOut.kind = 2;
        signOut.title = Label("Se déconnecter");
        signOut.action = ^{ [self confirmSignOut]; };
        account.rows.push_back(signOut);
        categories.push_back(account);
    }
    SettingsCategory about {Label("À propos"), @"info", {}};
    SettingsEntry updates;
    updates.kind = 2;
    updates.title = Label("Vérifier les mises à jour");
    updates.action = ^{ [self checkUpdates]; };
    about.rows.push_back(updates);
    // The version itself checks for updates too.
    SettingsEntry version;
    version.kind = 2;
    version.title = Text("LunaONE  " + lunanx::VersionLabel());
    version.action = ^{ [self checkUpdates]; };
    about.rows.push_back(version);
    for (const auto& line : {lunanx::Tr("Crédit") + "  " + lunanx::ProjectCredit, lunanx::Tr(lunanx::Disclaimer)})
    {
        SettingsEntry entry;
        entry.kind = 3;
        entry.title = Text(line);
        about.rows.push_back(entry);
    }
    categories.push_back(about);
    return categories;
}

- (void)showSettings:(NSInteger)focusChoice
{
    if (_busy) return;
    [self closePanel];
    _settings = YES;
    _settingsCategory = 0;
    _settingsColumn = 0;
    _settingsRow = 0;
    _settingsEditing = -1;
    _settingsFirst = 0;
    _settingsMotion = 3;
    [self render];
}

- (void)closeSettings
{
    _settings = NO;
    [self render];
}

// Focusable rows of a category: choices, switches and actions.
- (std::vector<int>)settingsTargets:(const SettingsCategory&)category
{
    std::vector<int> targets;
    for (std::size_t i = 0; i < category.rows.size(); ++i)
        if (category.rows[i].kind != 3) targets.push_back(static_cast<int>(i));
    return targets;
}

- (void)settingsActivate:(const SettingsEntry&)entry row:(int)row
{
    _settingsColumn = 1;
    _settingsRow = row;
    if (entry.kind == 2)
    {
        if (entry.action) entry.action();
        return;
    }
    if (entry.kind == 1)
    {
        [self applyChoice:entry.choice value:entry.on ? 0 : 1];
        return;
    }
    const auto choices = lunanx::PlaybackChoices(lunanx::LoadPlaybackPreferences(), true, true, true);
    _settingsEditing = entry.choice;
    _settingsValue = static_cast<int>(choices.at(entry.choice).selected);
    _settingsColumn = 2;
    _settingsMotion = 1;
    [self render];
}

- (void)settingsMove:(Move)move
{
    const auto categories = [self settingsCategories];
    _settingsCategory = std::clamp(_settingsCategory, 0, static_cast<int>(categories.size()) - 1);
    const auto& category = categories[_settingsCategory];
    const auto targets = [self settingsTargets:category];
    const auto step = [&](int direction) {
        auto at = std::ranges::find(targets, _settingsRow);
        if (at == targets.end()) { if (!targets.empty()) _settingsRow = targets.front(); return; }
        if (direction < 0 && at != targets.begin()) _settingsRow = *(at - 1);
        if (direction > 0 && at + 1 != targets.end()) _settingsRow = *(at + 1);
    };
    const auto changeCategory = [&](int index) {
        index = std::clamp(index, 0, static_cast<int>(categories.size()) - 1);
        if (index == _settingsCategory) return;
        _settingsCategory = index;
        _settingsEditing = -1;
        _settingsFirst = 0;
        _settingsMotion = 3;
    };
    switch (move)
    {
    case Move::PreviousSection: changeCategory(_settingsCategory - 1); _settingsColumn = 0; break;
    case Move::NextSection: changeCategory(_settingsCategory + 1); _settingsColumn = 0; break;
    case Move::Menu: [self closeSettings]; return;
    default: break;
    }
    if (move == Move::PreviousSection || move == Move::NextSection) { [self render]; return; }
    // Column -1: the « Fermer » button, above the first category.
    if (_settingsColumn == -1)
    {
        if (move == Move::Activate || move == Move::Back) { [self closeSettings]; return; }
        if (move == Move::Down || move == Move::Left) _settingsColumn = 0;
    }
    else if (_settingsColumn == 0)
    {
        if (move == Move::Up && _settingsCategory == 0) _settingsColumn = -1;
        else if (move == Move::Up) changeCategory(_settingsCategory - 1);
        else if (move == Move::Down) changeCategory(_settingsCategory + 1);
        else if ((move == Move::Right || move == Move::Activate) && !targets.empty())
        {
            _settingsColumn = 1;
            _settingsRow = targets.front();
        }
        else if (move == Move::Back || move == Move::Left) { [self closeSettings]; return; }
    }
    else if (_settingsColumn == 1)
    {
        if (move == Move::Up) step(-1);
        else if (move == Move::Down) step(1);
        else if (move == Move::Left || move == Move::Back) _settingsColumn = 0;
        else if (move == Move::Activate && _settingsRow < static_cast<int>(category.rows.size()))
        {
            [self settingsActivate:category.rows[_settingsRow] row:_settingsRow];
            return;
        }
    }
    else
    {
        const auto choices = lunanx::PlaybackChoices(lunanx::LoadPlaybackPreferences(), true, true, true);
        const int count = _settingsEditing >= 0 ? static_cast<int>(choices.at(_settingsEditing).labels.size()) : 0;
        if (move == Move::Up) _settingsValue = std::max(0, _settingsValue - 1);
        else if (move == Move::Down) _settingsValue = std::min(count - 1, _settingsValue + 1);
        else if (move == Move::Left || move == Move::Back)
        {
            _settingsColumn = 1;
            _settingsEditing = -1;
            _settingsMotion = 2;
        }
        else if (move == Move::Activate && _settingsEditing >= 0)
        {
            const int choice = _settingsEditing;
            _settingsColumn = 1;
            _settingsEditing = -1;
            _settingsMotion = 2;
            [self applyChoice:choice value:_settingsValue];
            return;
        }
    }
    [self render];
}

- (void)renderSettings
{
    [_settingsView removeFromSuperview];
    _settingsView = nil;
    if (!_settings) return;
    const auto categories = [self settingsCategories];
    _settingsCategory = std::clamp(_settingsCategory, 0, static_cast<int>(categories.size()) - 1);
    const auto& category = categories[_settingsCategory];
    const bool keys = _device != Device::Mouse;
    __weak LunaCatalog* weak = self;

    // Opaque, with the library's dark violet going darker downwards.
    auto screen = [self control:_root.bounds in:_root clicked:^(NSInteger) {}];
    screen.layer.backgroundColor = Argb(0xff100d1d).CGColor;
    auto gradient = [CAGradientLayer layer];
    gradient.frame = screen.bounds;
    gradient.colors = @[(__bridge id)Argb(0xff1a1530).CGColor, (__bridge id)Argb(0xff0b0914).CGColor];
    gradient.startPoint = CGPointMake(.5, 0);
    gradient.endPoint = CGPointMake(.5, 1);
    [screen.layer insertSublayer:gradient atIndex:0];
    _settingsView = screen;
    [self put:[self label:Label("Réglages") size:30 color:kText bold:YES] in:screen
        frame:[self x:64 y:32 w:500 h:50]];
    auto close = [self control:[self x:_width - 64 - 150 y:34 w:150 h:46] in:screen clicked:^(NSInteger) {
        [weak closeSettings];
    }];
    const bool closeFocused = keys && _settingsColumn == -1;
    [self paint:close color:closeFocused ? 0x33aa88ff : kShell radius:23 stroke:closeFocused ? 3 : 1
    strokeColor:closeFocused ? kViolet : kRing];
    auto closeLabel = [self label:Label("Fermer") size:16 color:kText bold:YES];
    closeLabel.alignment = NSTextAlignmentCenter;
    [self put:closeLabel in:close frame:close.bounds];

    // Left: the categories.
    for (std::size_t i = 0; i < categories.size(); ++i)
    {
        const bool chosen = static_cast<int>(i) == _settingsCategory;
        const bool focused = chosen && keys && _settingsColumn == 0;
        auto item = [self control:[self x:64 y:112 + i * 62 w:300 h:54] in:screen clicked:^(NSInteger) {
            LunaCatalog* catalog = weak;
            if (!catalog) return;
            if (catalog->_settingsCategory != static_cast<int>(i)) catalog->_settingsMotion = 3;
            catalog->_settingsCategory = static_cast<int>(i);
            catalog->_settingsColumn = 0;
            catalog->_settingsEditing = -1;
            catalog->_settingsFirst = 0;
            [catalog render];
        }];
        [self paint:item color:chosen ? 0x33aa88ff : 0x00000000 radius:14 stroke:focused ? 2 : 0 strokeColor:kViolet];
        if (chosen) [self shape:kViolet radius:2 stroke:0 strokeColor:0 frame:[self local:0 y:14 w:4 h:26] in:item];
        auto icon = [[LunaIcon alloc] initWithFrame:[self local:20 y:15 w:24 h:24]];
        icon.type = categories[i].icon;
        icon.color = Argb(chosen ? kViolet : kText);
        [item addSubview:icon];
        [self put:[self label:categories[i].title size:18 color:chosen ? kText : kSecondary bold:chosen] in:item
            frame:[self local:58 y:0 w:230 h:54]];
    }
    [self shape:0x26ffffff radius:0 stroke:0 strokeColor:0 frame:[self x:392 y:112 w:1 h:540] in:screen];

    // Right: the rows of the category, or the values of the row being chosen.
    const CGFloat paneX = 424, paneWidth = _width - paneX - 64, paneHeight = 548;
    auto pane = [[LunaFlippedView alloc] initWithFrame:[self x:paneX y:104 w:paneWidth h:paneHeight]];
    pane.wantsLayer = YES;
    pane.layer.masksToBounds = YES;
    [screen addSubview:pane];
    if (_settingsEditing >= 0)
    {
        const auto choices = lunanx::PlaybackChoices(lunanx::LoadPlaybackPreferences(), true, true, true);
        const auto& choice = choices.at(_settingsEditing);
        auto back = [self control:[self local:0 y:8 w:paneWidth h:44] in:pane clicked:^(NSInteger) {
            [weak settingsMove:Move::Back];
        }];
        [self put:[self label:[@"‹   " stringByAppendingString:Text(choice.title)] size:22 color:kText bold:YES] in:back
            frame:back.bounds];
        const int visible = 7;
        const int count = static_cast<int>(choice.labels.size());
        int first = std::clamp(_settingsValue - visible + 1, 0, std::max(0, count - visible));
        if (_settingsValue < first) first = _settingsValue;
        for (int v = first; v < std::min(count, first + visible); ++v)
        {
            const bool current = static_cast<std::size_t>(v) == choice.selected;
            const bool focused = v == _settingsValue && keys;
            const int index = _settingsEditing;
            auto row = [self control:[self local:0 y:64 + (v - first) * 62 w:paneWidth h:54] in:pane clicked:^(NSInteger) {
                LunaCatalog* catalog = weak;
                if (!catalog) return;
                catalog->_settingsValue = v;
                catalog->_settingsColumn = 1;
                catalog->_settingsEditing = -1;
                catalog->_settingsMotion = 2;
                [catalog applyChoice:index value:v];
            }];
            [self paint:row color:focused ? 0x33aa88ff : 0x12ffffff radius:14 stroke:focused ? 2 : 0 strokeColor:kViolet];
            // Radio mark: filled for the current value.
            auto mark = [self shape:current ? kViolet : 0x00000000 radius:10 stroke:2 strokeColor:current ? kViolet : kRing
                              frame:[self local:20 y:17 w:20 h:20] in:row];
            if (current)
                [self shape:0xffffffff radius:4 stroke:0 strokeColor:0 frame:[self local:6 y:6 w:8 h:8] in:mark];
            [self put:[self label:Text(choice.labels[v]) size:18 color:current ? kText : kSecondary bold:current] in:row
                frame:[self local:56 y:0 w:paneWidth - 76 h:54]];
        }
        if (first > 0)
            [self put:[self label:@"▲" size:12 color:kSecondary bold:NO] in:pane frame:[self local:paneWidth - 24 y:20 w:20 h:20]];
        if (first + visible < count)
            [self put:[self label:@"▼" size:12 color:kSecondary bold:NO] in:pane
                frame:[self local:paneWidth - 24 y:paneHeight - 24 w:20 h:20]];
    }
    else
    {
        [self put:[self label:category.title size:22 color:kText bold:YES] in:pane frame:[self local:0 y:8 w:paneWidth h:44]];
        const auto height = [](const SettingsEntry& entry) -> CGFloat { return entry.kind == 3 ? 40 : 62; };
        // Keep the focused row inside the pane.
        if (_settingsColumn == 1)
        {
            _settingsFirst = std::min(_settingsFirst, _settingsRow);
            const auto fits = [&](int first) {
                CGFloat total = 0;
                for (int i = first; i <= _settingsRow; ++i) total += height(category.rows[i]);
                return total <= paneHeight - 64;
            };
            while (_settingsFirst < _settingsRow && !fits(_settingsFirst)) ++_settingsFirst;
        }
        _settingsFirst = std::clamp(_settingsFirst, 0, std::max(0, static_cast<int>(category.rows.size()) - 1));
        CGFloat y = 64;
        int shownUntil = _settingsFirst;
        for (int i = _settingsFirst; i < static_cast<int>(category.rows.size()) && y + height(category.rows[i]) - 8 <= paneHeight; ++i)
        {
            shownUntil = i + 1;
            const auto& entry = category.rows[i];
            if (entry.kind == 3)
            {
                [self put:[self label:entry.title size:15 color:kSecondary bold:NO] in:pane frame:[self local:20 y:y w:paneWidth - 40 h:40]];
                y += 40;
                continue;
            }
            const bool focused = keys && _settingsColumn == 1 && i == _settingsRow;
            const SettingsEntry copy = entry;
            auto row = [self control:[self local:0 y:y w:paneWidth h:54] in:pane clicked:^(NSInteger) {
                [weak settingsActivate:copy row:i];
            }];
            [self paint:row color:focused ? 0x33aa88ff : 0x12ffffff radius:14 stroke:focused ? 2 : 0 strokeColor:kViolet];
            const bool accent = entry.kind == 2;
            [self put:[self label:entry.title size:18 color:accent ? kViolet : kText bold:entry.kind == 2] in:row
                frame:[self local:20 y:0 w:paneWidth * .55 h:54]];
            if (entry.kind == 1)
            {
                // A switch: violet and to the right when on.
                auto track = [self shape:entry.on ? kViolet : 0x40ffffff radius:15 stroke:0 strokeColor:0
                                   frame:[self local:paneWidth - 76 y:12 w:54 h:30] in:row];
                [self shape:0xffffffff radius:12 stroke:0 strokeColor:0
                      frame:[self local:entry.on ? 27 : 3 y:3 w:24 h:24] in:track];
            }
            else if (entry.kind == 0)
            {
                auto value = [self label:[entry.value stringByAppendingString:@"   ›"] size:15 color:kSecondary bold:NO];
                value.alignment = NSTextAlignmentRight;
                [self put:value in:row frame:[self local:paneWidth * .5 y:0 w:paneWidth * .5 - 22 h:54]];
            }
            else
            {
                auto arrow = [self label:@"›" size:20 color:kViolet bold:YES];
                arrow.alignment = NSTextAlignmentRight;
                [self put:arrow in:row frame:[self local:paneWidth - 60 y:0 w:40 h:54]];
            }
            y += 62;
        }
        if (_settingsFirst > 0)
            [self put:[self label:@"▲" size:12 color:kSecondary bold:NO] in:pane frame:[self local:paneWidth - 24 y:20 w:20 h:20]];
        if (shownUntil < static_cast<int>(category.rows.size()))
            [self put:[self label:@"▼" size:12 color:kSecondary bold:NO] in:pane
                frame:[self local:paneWidth / 2 - 10 y:paneHeight - 22 w:20 h:20]];
    }

    // The right pane slides in: from the right going deeper, from the left coming back.
    if (_settingsMotion && !NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion)
    {
        const CGFloat shift = (_settingsMotion == 1 ? 36 : _settingsMotion == 2 ? -36 : 0) * _scale;
        auto fade = [CABasicAnimation animationWithKeyPath:@"opacity"];
        fade.fromValue = @0;
        fade.toValue = @1;
        auto slide = [CABasicAnimation animationWithKeyPath:@"transform.translation"];
        slide.fromValue = [NSValue valueWithSize:NSMakeSize(shift, _settingsMotion == 3 ? 10 * _scale : 0)];
        slide.toValue = [NSValue valueWithSize:NSZeroSize];
        auto group = [CAAnimationGroup animation];
        group.animations = @[fade, slide];
        group.duration = .22;
        group.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
        [pane.layer addAnimation:group forKey:@"enter"];
    }
    _settingsMotion = 0;

    // Hints for the device in use.
    std::string hint;
    if (_device == Device::Gamepad)
    {
        using lunanx::input::PadButton;
        GCController* controller = lunaone::apple::ActiveGamepad();
        const bool swapped = lunanx::LoadPlaybackPreferences().swap_ab;
        hint = lunaone::apple::ButtonLabel(controller, swapped ? PadButton::B : PadButton::A) + "  " + lunanx::Tr("Valider") +
               "    " + lunaone::apple::ButtonLabel(controller, swapped ? PadButton::A : PadButton::B) + "  " +
               lunanx::Tr("Retour") + "    " + lunanx::input::PadButtonName(lunaone::apple::ControllerFamilyOf(controller),
               PadButton::LeftShoulder) + " / " + lunanx::input::PadButtonName(lunaone::apple::ControllerFamilyOf(controller),
               PadButton::RightShoulder) + "  " + lunanx::Tr("Rubriques");
    }
    else
        hint = "↑ ↓ ← →  " + lunanx::Tr("Naviguer") + "    ↩  " + lunanx::Tr("Valider") + "    ⎋  " + lunanx::Tr("Retour");
    [self put:[self label:Text(hint) size:13 color:kSecondary bold:NO] in:screen frame:[self x:64 y:682 w:_width - 128 h:30]];
}

- (void)applyChoice:(std::size_t)index value:(std::size_t)value
{
    auto preferences = lunanx::LoadPlaybackPreferences();
    lunanx::PlaybackChoices(preferences, true, true, true).at(index).apply(preferences, value);
    if (!lunanx::SavePlaybackPreferences(preferences))
    {
        _message = Label("Impossible d’enregistrer les réglages vidéo.");
        return;
    }
    DeviceRegistration saved;
    lunanx::SetUiLanguageForMarket(AmazonAuth::Load(saved) ? saved.market_code : preferences.market);
    if (!preferences.market.empty() && !_saved) _market = preferences.market;
    if (lunaone::apple::IsBorderlessFullscreen(_window) != preferences.fullscreen)
        lunaone::apple::SetBorderlessFullscreen(_window, preferences.fullscreen);
    _simple = preferences.simple_library;
    [self buildMenu];
    [self render];
}

- (void)openPanel:(NSString*)title rows:(NSArray<LunaPanelRow*>*)rows back:(void (^)(void))back
{
    if (_panelField && ![title isEqualToString:Label("Rechercher un jeu")]) _panelField = nil;
    _panelTitle = title;
    _panelRows = rows;
    _panelBack = back;
    _panelFirst = 0;
    _panelFocus = -1;
    for (NSUInteger i = 0; i < rows.count && _panelFocus < 0; ++i)
        if (rows[i].kind == 0) _panelFocus = i;
    if (_panelFocus < 0 && rows.lastObject.kind == 4) _panelFocus = rows.count - 1;
    for (NSUInteger i = 0; i < rows.count; ++i)
        if (rows[i].checked) _panelFocus = i;
    [self renderPanel];
}

- (void)closePanel
{
    _keyboard.reset();
    _panelRows = nil;
    _panelTitle = nil;
    _panelBack = nil;
    _panelField = nil;
    [self renderPanel];
    [_window makeFirstResponder:_root];
}

- (CGFloat)panelHeight:(LunaPanelRow*)row width:(CGFloat)width
{
    switch (row.kind)
    {
    case 1: return 40;
    case 2: return 34;
    case 4: return 0;
    case 3:
    {
        auto label = [NSTextField wrappingLabelWithString:row.title];
        label.font = [NSFont systemFontOfSize:16];
        return std::ceil([label.cell cellSizeForBounds:NSMakeRect(0, 0, width, CGFLOAT_MAX)].height) + 14;
    }
    default: return 48;
    }
}

- (void)renderPanel
{
    [_panelView removeFromSuperview];
    _panelView = nil;
    if (!_panelRows) return;
    auto shade = [self control:_root.bounds in:_root clicked:^(NSInteger) {}];
    shade.layer.backgroundColor = [NSColor colorWithWhite:0 alpha:.58].CGColor;
    __weak LunaCatalog* weak = self;
    shade.clicked = ^(NSInteger) { LunaCatalog* catalog = weak; if (catalog && catalog->_panelBack) catalog->_panelBack(); };
    _panelView = shade;

    if (!_panelField) _keyboard.reset();
    const bool editing = [_window.firstResponder isKindOfClass:NSText.class];
    const CGFloat width = _keyboard ? 900 : 640, inner = width - 56, limit = _keyboard ? 680 : 600;
    const CGFloat fieldHeight = _panelField ? 62 + [self keyboardHeight] : 0;
    NSMutableArray<NSNumber*>* buttons = [NSMutableArray new];
    for (NSUInteger i = 0; i < _panelRows.count; ++i)
        if (_panelRows[i].kind == 4) [buttons addObject:@(i)];
    const CGFloat bar = buttons.count ? 78 : 0;
    // Keep the focused row visible: rows are shown from _panelFirst while they fit.
    const auto fits = [&](NSInteger first, NSInteger last) {
        CGFloat total = 0;
        for (NSInteger i = first; i <= last; ++i) total += [self panelHeight:_panelRows[i] width:inner];
        return total <= limit - 84 - fieldHeight - bar;
    };
    if (_panelFocus >= 0 && _panelFocus < _panelFirst) _panelFirst = _panelFocus;
    const NSInteger listFocus = _panelFocus >= 0 && _panelRows[_panelFocus].kind == 4 ? -1 : _panelFocus;
    if (listFocus < 0 && _panelFocus >= 0) _panelFirst = std::min<NSInteger>(_panelFirst, 0);
    while (listFocus >= 0 && !fits(_panelFirst, listFocus)) ++_panelFirst;
    // Show the heading above the first visible row.
    if (_panelFirst > 0 && _panelRows[_panelFirst - 1].kind == 1 && fits(_panelFirst - 1, _panelFocus)) --_panelFirst;
    NSInteger last = _panelFirst;
    CGFloat height = 0;
    while (last < static_cast<NSInteger>(_panelRows.count) && fits(_panelFirst, last))
        height += [self panelHeight:_panelRows[last++] width:inner];
    const CGFloat cardHeight = 84 + fieldHeight + height + bar + 18;
    const CGFloat left = (_width - width) / 2, top = std::max<CGFloat>(12, (720 - cardHeight) / 2);
    auto card = [self control:[self x:left y:top w:width h:cardHeight] in:shade clicked:^(NSInteger) {}];
    [self paint:card color:0xff1a1530 radius:22 stroke:1 strokeColor:0x668e80a6];
    [self put:[self label:_panelTitle size:24 color:kText bold:YES] in:card frame:[self local:28 y:22 w:inner h:40]];
    if (_panelFirst > 0)
        [self put:[self label:@"▲" size:12 color:kSecondary bold:NO] in:card frame:[self local:width - 44 y:30 w:20 h:20]];
    if (last < static_cast<NSInteger>(_panelRows.count))
        [self put:[self label:@"▼" size:12 color:kSecondary bold:NO] in:card
            frame:[self local:width - 44 y:cardHeight - 30 w:20 h:20]];
    CGFloat y = 76;
    if (_panelField)
    {
        // A rounded box with the text centred in it and a margin on the left.
        auto box = [self shape:kCard radius:12 stroke:2 strokeColor:kViolet frame:[self local:28 y:y w:inner h:50] in:card];
        _panelField.font = [self font:19 bold:NO];
        _panelField.bezeled = NO;
        _panelField.bordered = NO;
        _panelField.drawsBackground = NO;
        _panelField.textColor = Argb(kText);
        _panelField.focusRingType = NSFocusRingTypeNone;
        _panelField.usesSingleLineMode = YES;
        _panelField.cell.scrollable = YES;
        _panelField.cell.wraps = NO;
        _panelField.placeholderAttributedString = [[NSAttributedString alloc] initWithString:Label("Titre du jeu")
            attributes:@{NSForegroundColorAttributeName: Argb(kSecondary), NSFontAttributeName: [self font:19 bold:NO]}];
        const CGFloat textHeight = std::ceil(_panelField.cell.cellSize.height);
        _panelField.frame = NSMakeRect(16 * _scale, (box.bounds.size.height - textHeight) / 2,
                                       box.bounds.size.width - 32 * _scale, textHeight);
        [box addSubview:_panelField];
        if (_keyboard) [self renderKeyboardIn:card width:width y:y + 62];
        y += fieldHeight;
        // Rebuilding the panel must not take the typing focus away.
        if (editing) [self editField];
    }
    const bool keys = _device != Device::Mouse;
    for (NSInteger i = _panelFirst; i < last; ++i)
    {
        LunaPanelRow* row = _panelRows[i];
        const CGFloat rowHeight = [self panelHeight:row width:inner];
        if (row.kind == 4) continue;
        if (row.kind == 1)
            [self put:[self label:row.title size:14 color:kViolet bold:YES] in:card frame:[self local:30 y:y + 10 w:inner h:30]];
        else if (row.kind == 2)
            [self put:[self label:row.title size:15 color:kSecondary bold:NO] in:card frame:[self local:30 y:y w:inner h:rowHeight]];
        else if (row.kind == 3)
            [self paragraph:row.title size:16 color:kText bold:NO in:card frame:[self local:30 y:y + 4 w:inner h:rowHeight] centred:NO];
        else
        {
            auto control = [self control:[self local:20 y:y + 2 w:width - 40 h:rowHeight - 4] in:card
                                 clicked:^(NSInteger) {
                LunaCatalog* catalog = weak;
                if (!catalog) return;
                catalog->_panelFocus = i;
                if (row.action) row.action();
            }];
            const bool focused = i == _panelFocus && (keys || !_panelField);
            [self paint:control color:focused ? 0x40aa88ff : 0x00000000 radius:12 stroke:focused && keys ? 2 : 0
            strokeColor:kViolet];
            [self put:[self label:[(row.checked ? @"✓  " : @"") stringByAppendingString:row.title] size:17
                            color:row.checked ? kViolet : kText bold:row.checked]
                   in:control frame:[self local:12 y:0 w:inner * .62 h:rowHeight - 4]];
            if (row.value.length)
            {
                auto value = [self label:[row.value stringByAppendingString:@"   ›"] size:15 color:kSecondary bold:NO];
                value.alignment = NSTextAlignmentRight;
                [self put:value in:control frame:[self local:inner * .62 y:0 w:width - 52 - inner * .62 h:rowHeight - 4]];
            }
        }
        y += rowHeight;
    }
    // Buttons along the bottom: the first on the left, the main one on the far right.
    CGFloat right = width - 28;
    for (NSUInteger n = 0; n < buttons.count; ++n)
    {
        const NSInteger i = buttons[buttons.count - 1 - n].integerValue;
        LunaPanelRow* row = _panelRows[i];
        const bool primary = n == 0;
        auto label = [self label:row.title size:17 color:primary ? kInk : kText bold:YES];
        label.alignment = NSTextAlignmentCenter;
        const CGFloat buttonWidth = std::max<CGFloat>(150, label.cell.cellSize.width / _scale + 48);
        const bool leftmost = n + 1 == buttons.count && buttons.count > 1;
        const CGFloat x = leftmost ? 28 : right - buttonWidth;
        right = x - 14;
        auto control = [self control:[self local:x y:cardHeight - 22 - 52 w:buttonWidth h:52] in:card clicked:^(NSInteger) {
            LunaCatalog* catalog = weak;
            if (!catalog) return;
            catalog->_panelFocus = i;
            if (row.action) row.action();
        }];
        const bool focused = i == _panelFocus && keys && ![_window.firstResponder isKindOfClass:NSText.class];
        [self paint:control color:primary ? kPill : kShell radius:26 stroke:focused ? 3 : (primary ? 0 : 1)
        strokeColor:focused ? kViolet : kRing];
        [self put:label in:control frame:control.bounds];
    }
}

// L1 + L2 + R1 + R2: shows or hides the keyboard, opening the search first.
- (void)toggleKeyboard
{
    if (!_client || _busy || _loading) return;
    if (!lunanx::LoadPlaybackPreferences().virtual_keyboard && !_keyboard) return;
    if (!_panelField)
    {
        [self showSearch];
        if (!_panelField) return;
        _keyboard = std::make_unique<lunanx::ui::VirtualKeyboard>(lunanx::UiLanguage());
    }
    else if (_keyboard) _keyboard.reset();
    else _keyboard = std::make_unique<lunanx::ui::VirtualKeyboard>(lunanx::UiLanguage());
    lunanx::LogRuntimeEvent("ui", "virtual_keyboard", _keyboard ? "shown" : "hidden");
    [self renderPanel];
}

- (void)keyboardMove:(Move)move
{
    std::string text = _panelField.stringValue.UTF8String ?: "";
    switch (move)
    {
    case Move::Left: _keyboard->Move(-1, 0); break;
    case Move::Right: _keyboard->Move(1, 0); break;
    case Move::Up: _keyboard->Move(0, -1); break;
    case Move::Down: _keyboard->Move(0, 1); break;
    case Move::Search: _keyboard->ToggleShift(); break;
    case Move::Space: text += ' '; break;
    case Move::Activate:
        if (_keyboard->Press(text))
        {
            _panelField.stringValue = Text(text);
            [self submitSearch:nil];
            return;
        }
        break;
    case Move::Back:
        // Delete, or hide the keyboard once the field is empty.
        if (text.empty()) _keyboard.reset();
        else lunanx::ui::VirtualKeyboard::Backspace(text);
        break;
    case Move::Menu:
        [self submitSearch:nil];
        return;
    default: return;
    }
    _panelField.stringValue = Text(text);
    [self renderPanel];
}

- (CGFloat)keyboardHeight
{
    return _keyboard ? _keyboard->Rows().size() * 56 + 44 : 0;
}

- (void)renderKeyboardIn:(NSView*)card width:(CGFloat)width y:(CGFloat)y
{
    const auto& rows = _keyboard->Rows();
    const CGFloat unit = 64, gap = 6, height = 50;
    __weak LunaCatalog* weak = self;
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        int units = 0;
        for (const auto& key : rows[r]) units += key.span;
        CGFloat x = (width - (units * unit - gap)) / 2;
        for (std::size_t c = 0; c < rows[r].size(); ++c)
        {
            const auto& key = rows[r][c];
            const CGFloat keyWidth = key.span * unit - gap;
            auto control = [self control:[self local:x y:y w:keyWidth h:height] in:card clicked:^(NSInteger) {
                LunaCatalog* catalog = weak;
                if (!catalog || !catalog->_keyboard) return;
                catalog->_keyboard->Focus(r, c);
                [catalog keyboardMove:Move::Activate];
            }];
            const bool focused = r == _keyboard->Row() && c == _keyboard->Column();
            const bool active = key.kind == lunanx::ui::KeyKind::Shift && _keyboard->Shifted();
            [self paint:control color:focused ? kFocus : active ? 0xff3a2c66 : kCard radius:10 stroke:focused ? 2 : 0
            strokeColor:kViolet];
            auto label = [self label:Text(key.label) size:key.span > 1 ? 16 : 20 color:kText bold:YES];
            label.alignment = NSTextAlignmentCenter;
            [self put:label in:control frame:control.bounds];
            x += key.span * unit;
        }
        y += height + gap;
    }
    using lunanx::input::PadButton;
    const bool swapped = lunanx::LoadPlaybackPreferences().swap_ab;
    GCController* controller = lunaone::apple::ActiveGamepad();
    const auto name = [&](PadButton button) { return lunaone::apple::ButtonLabel(controller, button) + "  "; };
    const std::string hint = name(swapped ? PadButton::B : PadButton::A) + lunanx::Tr("Saisir") + "    " +
        name(swapped ? PadButton::A : PadButton::B) + lunanx::Tr("Effacer") + "    " + name(PadButton::X) +
        lunanx::Tr("Espace") + "    " + name(PadButton::Y) + lunanx::Tr("Maj") + "    " + name(PadButton::Menu) +
        lunanx::Tr("Valider") + "    " + lunanx::input::KeyboardChordName(lunaone::apple::ControllerFamilyOf(controller)) +
        "  " + lunanx::Tr("Masquer le clavier");
    auto label = [self label:Text(hint) size:13 color:kSecondary bold:NO];
    label.alignment = NSTextAlignmentCenter;
    [self put:label in:card frame:[self local:20 y:y + 4 w:width - 40 h:30]];
}

- (void)panelMove:(Move)move
{
    const auto count = static_cast<NSInteger>(_panelRows.count);
    const bool onButton = _panelFocus >= 0 && _panelRows[_panelFocus].kind == 4;
    // Next list row in a direction, or -1.
    const auto next = [&](NSInteger from, int direction) -> NSInteger {
        for (NSInteger i = from + direction; i >= 0 && i < count; i += direction)
            if (_panelRows[i].kind == 0) return i;
        return -1;
    };
    const auto button = [&](int direction) {
        for (NSInteger i = _panelFocus + direction; i >= 0 && i < count; i += direction)
            if (_panelRows[i].kind == 4) { _panelFocus = i; return; }
    };
    switch (move)
    {
    case Move::Up:
        if (!onButton) { if (const auto row = next(_panelFocus, -1); row >= 0) _panelFocus = row; }
        else if (const auto row = next(count, -1); row >= 0) _panelFocus = row;
        else if (_panelField) { [self editField]; [self renderPanel]; return; }
        break;
    case Move::Down:
        if (onButton) break;
        if (const auto row = next(_panelFocus, 1); row >= 0) _panelFocus = row;
        else if (_panelRows.lastObject.kind == 4) _panelFocus = count - 1;
        break;
    case Move::Left: if (onButton) button(-1); break;
    case Move::Right: if (onButton) button(1); break;
    case Move::Activate:
        if (_panelFocus >= 0 && _panelRows[_panelFocus].action) _panelRows[_panelFocus].action();
        return;
    case Move::Back:
    case Move::Menu:
        if (_panelBack) _panelBack();
        return;
    default: return;
    }
    [self renderPanel];
}

#pragma mark - Input

- (BOOL)key:(NSEvent*)event
{
    if (lunaone::apple::IsFullscreenShortcut(event))
    {
        [self toggleFullscreen:nil];
        return YES;
    }
    if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))
        return NO;
    Move move;
    switch (event.keyCode)
    {
    case 123: move = Move::Left; break;
    case 124: move = Move::Right; break;
    case 125: move = Move::Down; break;
    case 126: move = Move::Up; break;
    case 36: case 76: case 49: move = Move::Activate; break;
    case 51: move = Move::Back; break;
    case 53:
        // Esc closes an open panel, otherwise leaves full screen for the window.
        if (!_panelRows && !_settings && lunaone::apple::IsBorderlessFullscreen(_window))
        {
            [self toggleFullscreen:nil];
            return YES;
        }
        move = Move::Back;
        break;
    default: return NO;
    }
    [self usedDevice:Device::Keyboard];
    [self navigate:move];
    return YES;
}

- (void)wheel:(NSEvent*)event
{
    if (_settings && !_panelRows)
    {
        const CGFloat delta = event.scrollingDeltaY;
        if (std::abs(delta) < 1 || _settingsEditing >= 0) return;
        _settingsFirst = std::max(0, _settingsFirst + (delta > 0 ? -1 : 1));
        _settingsColumn = std::min(_settingsColumn, 1);
        [self render];
        return;
    }
    if (_panelRows)
    {
        const CGFloat delta = event.scrollingDeltaY;
        if (std::abs(delta) < 1) return;
        [self panelMove:delta > 0 ? Move::Up : Move::Down];
        return;
    }
    if (!_track) return;
    const CGFloat delta = std::abs(event.scrollingDeltaX) > std::abs(event.scrollingDeltaY) ? event.scrollingDeltaX
                                                                                         : event.scrollingDeltaY;
    const CGFloat points = (event.hasPreciseScrollingDeltas ? delta : delta * 24) / _scale;
    [self setTrackOffset:_trackOffset - points animated:NO];
}

- (void)navigate:(Move)move
{
    if (_panelRows)
    {
        if (_keyboard && _panelField) [self keyboardMove:move];
        else [self panelMove:move];
        return;
    }
    if (_settings)
    {
        [self settingsMove:move];
        return;
    }
    if (_loading)
    {
        if (move == Move::Activate || move == Move::Back) [self cancelJob];
        return;
    }
    if (!_client) { [self loginMove:move]; return; }
    if (_busy)
    {
        if (move == Move::Activate || move == Move::Back) [self cancelJob];
        return;
    }
    const bool searching = !_query.empty();
    // Section shown by the icon row.
    const int activeTop = searching ? 3 : std::min(_section, 2);
    switch (move)
    {
    case Move::Menu: [self showSettings:-1]; return;
    case Move::Search: [self showSearch]; return;
    case Move::PreviousSection:
    case Move::NextSection:
        [self openSection:(std::min(_section, 2) + (move == Move::NextSection ? 1 : 2)) % 3];
        return;
    case Move::Back:
        if (searching) [self openSection:_section];
        return;
    default: break;
    }
    if (_zone == kTop)
    {
        if (move == Move::Left) _topIndex = std::max(0, _topIndex - 1);
        else if (move == Move::Right) _topIndex = std::min<int>(static_cast<int>(_top.size()) - 1, _topIndex + 1);
        else if (move == Move::Down && !_covers.empty()) _zone = kCovers;
        else if (move == Move::Activate) { _top[_topIndex].clicked(1); return; }
    }
    else if (_zone == kCovers)
    {
        const auto back = _simple ? Move::Up : Move::Left, forward = _simple ? Move::Down : Move::Right;
        const std::size_t last = _first + _covers.size() - 1;
        if (move == back)
        {
            if (_selected > _first) [self select:_selected - 1];
            else if (_first > 0) [self turnPage:-1];
            else if (_simple) { _zone = kTop; _topIndex = activeTop; }
        }
        else if (move == forward)
        {
            if (_selected < last) [self select:_selected + 1];
            else if (last + 1 < _shown.size()) [self turnPage:1];
            else if (_simple) { _zone = kActions; _actionIndex = kPlay; }
        }
        else if (move == Move::Up) { _zone = kTop; _topIndex = activeTop; }
        else if (move == Move::Down) { _zone = kActions; _actionIndex = kPlay; }
        else if (move == Move::Activate) { [self playSelected]; return; }
    }
    else
    {
        std::vector<int> order;
        for (int i : {kPlay, kPrevious, kNext})
            if (_actions[i] && !_actions[i].hidden) order.push_back(i);
        auto at = std::ranges::find(order, _actionIndex);
        if (at == order.end()) at = order.begin();
        if (move == Move::Left && at != order.begin()) _actionIndex = *(at - 1);
        else if (move == Move::Right && at + 1 != order.end()) _actionIndex = *(at + 1);
        else if (move == Move::Up && !_covers.empty()) _zone = kCovers;
        else if (move == Move::Activate && at != order.end()) { _actions[*at].clicked(1); return; }
    }
    [self refreshFocus];
}

- (void)loginMove:(Move)move
{
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < _loginControls.size(); ++i)
        if (!_loginControls[i].hidden && !_loginControls[i].disabled) visible.push_back(i);
    if (visible.empty()) return;
    auto at = std::ranges::find(visible, _loginFocus);
    if (at == visible.end()) at = visible.begin();
    if ((move == Move::Up || move == Move::Left) && at != visible.begin()) _loginFocus = *(at - 1);
    else if ((move == Move::Down || move == Move::Right) && at + 1 != visible.end()) _loginFocus = *(at + 1);
    else if (move == Move::Activate) { _loginControls[*at].clicked(1); return; }
    else if (move == Move::Back && _busy) { [self cancelJob]; return; }
    else if (move == Move::Menu) { [self showSettings:-1]; return; }
    else _loginFocus = *at;
    [self refreshFocus];
}

// Controllers drive the catalog as the TV remote does. The state is read
// rather than observed: the player installs its own handler on the same
// controller, and this timer does not run while a game is open.
- (void)pollGamepad
{
    GCController* controller = NSApp.isActive ? lunaone::apple::ActiveGamepad() : nil;
    if (!controller)
    {
        _pad = 0;
        _shoulders = 0;
        _chordHeld = false;
        return;
    }
    GCExtendedGamepad* pad = controller.extendedGamepad;
    const auto full = lunaone::apple::ReadGamepad(pad);
    const bool swapped = lunanx::LoadPlaybackPreferences().swap_ab;
    enum : std::uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kMenu = 64, kY = 128, kX = 256 };
    std::uint32_t state = 0;
    const auto set = [&](bool on, std::uint32_t bit) { if (on) state |= bit; };
    set(pad.dpad.up.isPressed || pad.leftThumbstick.yAxis.value > .55f, kUp);
    set(pad.dpad.down.isPressed || pad.leftThumbstick.yAxis.value < -.55f, kDown);
    set(pad.dpad.left.isPressed || pad.leftThumbstick.xAxis.value < -.55f, kLeft);
    set(pad.dpad.right.isPressed || pad.leftThumbstick.xAxis.value > .55f, kRight);
    set(swapped ? pad.buttonB.isPressed : pad.buttonA.isPressed, kA);
    set(swapped ? pad.buttonA.isPressed : pad.buttonB.isPressed, kB);
    set(pad.buttonMenu.isPressed, kMenu);
    set(pad.buttonY.isPressed, kY);
    set(pad.buttonX.isPressed, kX);
    // Shoulders and triggers: L1 and R1 alone change the section when they
    // are released; the four together show or hide the keyboard.
    const std::uint32_t shoulders = (full.buttons & lunanx::input::kLeftShoulder ? 1u : 0u) |
                                    (full.buttons & lunanx::input::kRightShoulder ? 2u : 0u) |
                                    (full.left_trigger >= 128 ? 4u : 0u) | (full.right_trigger >= 128 ? 8u : 0u);
    if (_padLatched)
    {
        _padLatched = state != 0 || shoulders != 0;
        _pad = state;
        _shoulders = 0;
        return;
    }
    if (lunanx::ui::VirtualKeyboardChord(full) && !_chordHeld)
    {
        _chordHeld = true;
        [self usedDevice:Device::Gamepad];
        [self toggleKeyboard];
    }
    if (shoulders)
        _shoulders |= shoulders;
    else if (_shoulders)
    {
        const auto gesture = _shoulders;
        const bool chord = _chordHeld;
        _shoulders = 0;
        _chordHeld = false;
        if (!chord && (gesture == 1 || gesture == 2))
        {
            [self usedDevice:Device::Gamepad];
            [self navigate:gesture == 1 ? Move::PreviousSection : Move::NextSection];
        }
    }
    const std::uint32_t pressed = state & ~_pad;
    _pad = state;
    if (_chordHeld) return;
    const NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    const std::pair<std::uint32_t, Move> directions[] = {
        {kUp, Move::Up}, {kDown, Move::Down}, {kLeft, Move::Left}, {kRight, Move::Right}};
    for (const auto& [bit, move] : directions)
    {
        if (pressed & bit)
        {
            [self usedDevice:Device::Gamepad];
            _repeatMove = static_cast<int>(bit);
            _repeatAt = now + .42;
            [self navigate:move];
        }
        else if ((state & bit) && _repeatMove == static_cast<int>(bit) && now >= _repeatAt)
        {
            _repeatAt = now + .11;
            [self navigate:move];
        }
    }
    const std::pair<std::uint32_t, Move> buttons[] = {
        {kA, Move::Activate}, {kB, Move::Back}, {kMenu, Move::Menu}, {kY, Move::Search}, {kX, Move::Space}};
    for (const auto& [bit, move] : buttons)
        if (pressed & bit)
        {
            [self usedDevice:Device::Gamepad];
            [self navigate:move];
            return;
        }
}
@end

int main()
{
    @autoreleasepool
    {
        try
        {
            const lunaone::desktop::AccountLock lock;
            const lunaone::desktop::CurlRuntime curl;
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            auto delegate = [LunaCatalog new];
            NSApp.delegate = delegate;
            [NSApp run];
            return 0;
        }
        catch (const std::exception& error)
        {
            auto alert = [NSAlert new];
            alert.messageText = @"LunaONE";
            alert.informativeText = Text(error.what());
            [alert runModal];
            return 1;
        }
    }
}
