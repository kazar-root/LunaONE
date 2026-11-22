// SPDX-License-Identifier: GPL-3.0-only

#include "macos_player.hpp"
#include "macos_window.hpp"
#include "playback_support.hpp"
#include "audio_output.hpp"
#include "video_decoder.hpp"
#include "metal_video_renderer.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "webrtc/luna_stream_session.hpp"
#include "stream/performance_overlay.hpp"
#include "stream/launch_status.hpp"

#import <AppKit/AppKit.h>
#include <mach/mach.h>

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

// Runs a block when a button is clicked.
@interface LunaButtonAction : NSObject
@property(nonatomic, copy) void (^block)(void);
- (void)fire:(id)sender;
@end

@implementation LunaButtonAction
- (void)fire:(id)sender
{
    if (self.block) self.block();
}
@end

@interface LunaPlaybackWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@end

@implementation LunaPlaybackWindowDelegate
- (void)windowWillClose:(NSNotification*)notification
{
    self.closed = YES;
}
@end

namespace lunaone::apple
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint64_t ResidentBytes()
{
    mach_task_basic_info_data_t info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const auto status = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                  reinterpret_cast<task_info_t>(&info), &count);
    return status == KERN_SUCCESS ? info.resident_size : 0;
}

struct Metrics
{
    Clock::time_point first_frame {};
    Clock::time_point last_frame {};
    Clock::time_point next_memory {};
    std::uint64_t displayed = 0;
    int decoded_height = 0;
    std::uint64_t peak_resident = 0;
    std::chrono::milliseconds max_frame_gap {};
    bool physical_controller = false;
};

void StopAppLoop()
{
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
                                        context:nil subtype:0 data1:0 data2:0] atStart:NO];
}

} // namespace

void Play(const lunanx::luna::LunaClient& client, lunanx::luna::Game game,
          std::chrono::seconds duration, std::function<bool()> cancelled, bool modal)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        auto iconURL = [NSBundle.mainBundle.executableURL.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"LunaONE.png"];
        if (auto icon = [[NSImage alloc] initWithContentsOfURL:iconURL])
            NSApp.applicationIconImage = icon;
        const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;
        // The game opens on the catalog's screen; the automatic video mode
        // reads that screen before the session asks Luna for a stream.
        NSScreen* screen = NSApp.keyWindow.screen ?: NSScreen.mainScreen;
        ReportDeviceDisplay(screen);
        auto window = [[LunaWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1024, 576) styleMask:style
                                                      backing:NSBackingStoreBuffered defer:NO screen:screen];
        window.releasedWhenClosed = NO;
        window.fullscreenChanged = ^(BOOL fullscreen) {
            auto saved = lunanx::LoadPlaybackPreferences();
            saved.fullscreen = fullscreen;
            lunanx::SavePlaybackPreferences(saved);
        };
        NSString* title = [NSString stringWithUTF8String:game.title.c_str()] ?: @"Game";
        window.title = [@"LunaONE — " stringByAppendingString:title];
        auto delegate = [[LunaPlaybackWindowDelegate alloc] init];
        window.delegate = delegate;
        const auto preferences = lunanx::LoadPlaybackPreferences();
        const auto filter = lunanx::PictureFilter(preferences.filter);
        // LunaNX picture path. AVFoundation shows the picture when Metal is unavailable.
        auto metal = MetalVideoRenderer::Create({filter.denoise, filter.sharpen});
        auto* renderer = metal.get();
        auto layer = [[AVSampleBufferDisplayLayer alloc] init];
        layer.videoGravity = AVLayerVideoGravityResizeAspect;
        layer.backgroundColor = NSColor.blackColor.CGColor;
        if (renderer)
            renderer->Layer().contentsScale = window.backingScaleFactor;
        window.contentView.layer = renderer ? renderer->Layer() : layer;
        window.contentView.wantsLayer = YES;
        auto hud = [NSTextField labelWithString:@""];
        hud.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightSemibold];
        hud.textColor = NSColor.whiteColor;
        hud.backgroundColor = [NSColor.blackColor colorWithAlphaComponent:0.68];
        hud.drawsBackground = YES;
        hud.translatesAutoresizingMaskIntoConstraints = NO;
        hud.hidden = YES;
        [window.contentView addSubview:hud];
        [NSLayoutConstraint activateConstraints:@[
            [hud.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:14],
            [hud.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-14]
        ]];
        // The launch screen keeps the catalog's background until the first picture.
        auto launchBackground = [[NSView alloc] init];
        launchBackground.wantsLayer = YES;
        launchBackground.layer.backgroundColor = [NSColor colorWithSRGBRed:16 / 255.0 green:13 / 255.0 blue:29 / 255.0
                                                                    alpha:1].CGColor;
        launchBackground.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:launchBackground];
        [NSLayoutConstraint activateConstraints:@[
            [launchBackground.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor],
            [launchBackground.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor],
            [launchBackground.topAnchor constraintEqualToAnchor:window.contentView.topAnchor],
            [launchBackground.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor]
        ]];
        auto launchLogo = [[NSImageView alloc] init];
        launchLogo.image = NSApp.applicationIconImage;
        launchLogo.translatesAutoresizingMaskIntoConstraints = NO;
        auto cover = [[NSImageView alloc] init];
        cover.imageScaling = NSImageScaleProportionallyUpOrDown;
        cover.wantsLayer = YES;
        cover.layer.backgroundColor = [NSColor colorWithSRGBRed:.12 green:.09 blue:.21 alpha:1].CGColor;
        cover.layer.cornerRadius = 12;
        cover.layer.masksToBounds = YES;
        const auto art = game.image_portrait.empty() ? game.image_landscape : game.image_portrait;
        FetchLaunchCover(art, ^(NSData* bytes) { cover.image = [[NSImage alloc] initWithData:bytes]; });
        auto gameName = [NSTextField wrappingLabelWithString:title];
        gameName.alignment = NSTextAlignmentCenter;
        gameName.font = [NSFont boldSystemFontOfSize:18];
        gameName.textColor = NSColor.whiteColor;
        auto launchText = [NSTextField labelWithString:[NSString stringWithUTF8String:lunanx::LaunchStatusText({}, 1).c_str()]];
        launchText.textColor = NSColor.whiteColor;
        auto progress = [NSStackView new];
        progress.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        progress.spacing = 8;
        for (int i = 0; i < 3; ++i)
        {
            auto square = [NSView new];
            square.wantsLayer = YES;
            square.layer.cornerRadius = 2;
            square.layer.backgroundColor = (i == 0 ? NSColor.whiteColor : [NSColor colorWithWhite:1 alpha:.25]).CGColor;
            [square.widthAnchor constraintEqualToConstant:11].active = YES;
            [square.heightAnchor constraintEqualToConstant:11].active = YES;
            [progress addArrangedSubview:square];
        }
        auto exitHint = [NSTextField labelWithString:
            [NSString stringWithUTF8String:lunanx::TrFormat("Quitter une partie : {}",
                {lunanx::input::ExitChordName(ControllerFamilyOf(ActiveGamepad()))}).c_str()] ?: @""];
        exitHint.textColor = [NSColor colorWithSRGBRed:.78 green:.74 blue:.83 alpha:1];
        auto launch = [NSStackView stackViewWithViews:@[cover, gameName, launchText, progress, exitHint]];
        launch.orientation = NSUserInterfaceLayoutOrientationVertical;
        launch.alignment = NSLayoutAttributeCenterX;
        launch.spacing = 10;
        launch.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:launchLogo];
        [window.contentView addSubview:launch];
        [NSLayoutConstraint activateConstraints:@[
            [launchLogo.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:18],
            [launchLogo.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:18],
            [launchLogo.widthAnchor constraintEqualToConstant:45],
            [launchLogo.heightAnchor constraintEqualToConstant:45],
            [cover.widthAnchor constraintEqualToConstant:146],
            [cover.heightAnchor constraintEqualToConstant:192],
            [launch.centerXAnchor constraintEqualToAnchor:window.contentView.centerXAnchor],
            [launch.centerYAnchor constraintEqualToAnchor:window.contentView.centerYAnchor],
            [gameName.widthAnchor constraintLessThanOrEqualToConstant:500],
        ]];
        // Bottom right of the launch screen: back to the catalog before the
        // game starts, once « Oui » confirms it. B opens the same question;
        // there A answers Oui and B Non.
        auto cancelAction = [LunaButtonAction new];
        cancelAction.block = ^{
            if (window.attachedSheet) return;
            NSAlert* confirm = [NSAlert new];
            confirm.messageText = [NSString stringWithUTF8String:lunanx::Tr("Annuler le lancement du jeu").c_str()] ?: @"";
            auto question = lunanx::Tr("Êtes-vous sûr ?");
            if (GCController* pad = ActiveGamepad())
            {
                const auto family = ControllerFamilyOf(pad);
                question += "\n\n" + lunanx::TrFormat("{} : Oui    {} : Non",
                    {lunanx::input::PadButtonName(family, lunanx::input::PadButton::A),
                     lunanx::input::PadButtonName(family, lunanx::input::PadButton::B)});
            }
            confirm.informativeText = [NSString stringWithUTF8String:question.c_str()] ?: @"";
            [confirm addButtonWithTitle:[NSString stringWithUTF8String:lunanx::Tr("Non").c_str()] ?: @"Non"];
            [confirm addButtonWithTitle:[NSString stringWithUTF8String:lunanx::Tr("Oui").c_str()] ?: @"Oui"];
            [confirm beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
                if (response == NSAlertSecondButtonReturn) [window close];
            }];
        };
        auto cancelLaunch = [NSButton buttonWithTitle:@"" target:cancelAction action:@selector(fire:)];
        cancelLaunch.bordered = NO;
        cancelLaunch.wantsLayer = YES;
        cancelLaunch.layer.backgroundColor = [NSColor colorWithSRGBRed:24 / 255.0 green:20 / 255.0 blue:43 / 255.0 alpha:.92].CGColor;
        cancelLaunch.layer.cornerRadius = 22;
        cancelLaunch.layer.borderWidth = 1;
        cancelLaunch.layer.borderColor = [NSColor colorWithSRGBRed:.49 green:.44 blue:.56 alpha:.6].CGColor;
        cancelLaunch.attributedTitle = [[NSAttributedString alloc]
            initWithString:[NSString stringWithUTF8String:lunanx::Tr("Annuler le lancement du jeu").c_str()] ?: @""
                attributes:@{NSForegroundColorAttributeName: NSColor.whiteColor,
                             NSFontAttributeName: [NSFont systemFontOfSize:15 weight:NSFontWeightBold]}];
        cancelLaunch.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:cancelLaunch];
        [NSLayoutConstraint activateConstraints:@[
            [cancelLaunch.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-28],
            [cancelLaunch.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-28],
            [cancelLaunch.heightAnchor constraintEqualToConstant:44],
            [cancelLaunch.widthAnchor constraintGreaterThanOrEqualToConstant:280]
        ]];
        const auto launching = std::make_shared<std::atomic<bool>>(true);
        auto launchStage = std::make_shared<int>(1);
        [window center];
        if (preferences.fullscreen)
            SetBorderlessFullscreen(window, true);

        __block Metrics metrics;
        // Pending main-queue blocks must not touch the decoder once playback has ended.
        auto alive = std::make_shared<bool>(true);
        __block VideoDecoder* frames = nullptr;
        // Show each picture as soon as VideoToolbox returns it rather than on
        // the next tick of the 90 Hz housekeeping timer.
        void (^present)(void) = ^{
            if (!*alive || !frames)
                return;
            auto image = frames->TakeFrame();
            if (!image)
                return;
            if (renderer ? renderer->Render(image) : Present(layer, image))
            {
                const auto now = Clock::now();
                metrics.decoded_height = static_cast<int>(CVPixelBufferGetHeight(image));
                if (metrics.displayed++ == 0)
                {
                    launch.hidden = launchLogo.hidden = launchBackground.hidden = cancelLaunch.hidden = YES;
                    // The game started: a pending confirmation means « Non ».
                    if (window.attachedSheet)
                        [window endSheet:window.attachedSheet returnCode:NSAlertFirstButtonReturn];
                    launching->store(false);
                    [NSCursor setHiddenUntilMouseMoves:YES];
                    metrics.first_frame = now;
                    std::cerr << "First picture queued for display\n";
                }
                else
                {
                    metrics.max_frame_gap = std::max(metrics.max_frame_gap,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - metrics.last_frame));
                }
                metrics.last_frame = now;
            }
            CFRelease(image);
        };
        const auto acceleration = preferences.decoder == lunanx::PlaybackDecoder::Hardware ? VideoDecoder::Acceleration::Hardware :
                                  preferences.decoder == lunanx::PlaybackDecoder::Software ? VideoDecoder::Acceleration::Software :
                                  VideoDecoder::Acceleration::Automatic;
        VideoDecoder video([present] { dispatch_async(dispatch_get_main_queue(), present); },
                           VideoDecoder::Mode::Asynchronous, acceleration);
        frames = &video;
        AudioOutput audio;
        const auto began = Clock::now();
        lunanx::LunaStreamSession session(client, std::move(game), {
            [&](auto bytes, auto timestamp) { return video.Submit(bytes, timestamp); },
            [&](auto bytes, auto sequence) { audio.Submit(bytes, sequence); },
            [launch, launchText, launchStage](const std::string& status) {
                @autoreleasepool
                {
                    std::cerr << status << '\n';
                    const int stage = lunanx::LaunchStage(status);
                    auto text = [NSString stringWithUTF8String:lunanx::LaunchStatusText(status, stage).c_str()];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (launch.hidden || stage < *launchStage) return;
                        *launchStage = stage;
                        launchText.stringValue = text;
                    });
                }
            }});
        const auto overlay = std::make_shared<lunanx::PerformanceOverlay>(preferences.diagnostics);
        const auto exitRequested = std::make_shared<std::atomic<bool>>(false);
        __block Clock::time_point nextHud;
        __block int pulse = 0;
        NetworkStatus network;
        auto* current_network = &network;
        __block GCController* previous_controller = nil;
        auto* active_session = &session;
        // Declared after the session so that it stops delivering before the session goes away.
        GamepadFeed gamepads([active_session, overlay, exitRequested, launching, window, cancelAction,
                              previous = std::uint32_t {0}](const lunanx::input::GamepadState& pad) mutable {
            const std::uint32_t pressed = pad.buttons & ~previous;
            previous = pad.buttons;
            if (lunanx::input::ExitStreamChord(pad))
            {
                exitRequested->store(true);
                return;
            }
            // Launch screen: B asks « Êtes-vous sûr ? », where A answers Oui and B Non.
            if (launching->load() && (pressed & (lunanx::input::kA | lunanx::input::kB)))
            {
                const bool yes = pressed & lunanx::input::kA;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (NSWindow* sheet = window.attachedSheet)
                        [window endSheet:sheet returnCode:yes ? NSAlertSecondButtonReturn : NSAlertFirstButtonReturn];
                    else if (!yes)
                        cancelAction.block();
                });
            }
            active_session->SetGamepad(overlay->Update(pad));
        });
        auto* gamepad_feed = &gamepads;
        auto timer = [NSTimer timerWithTimeInterval:1.0 / 90.0 repeats:YES block:^(NSTimer*) {
            @autoreleasepool
            {
                const auto now = Clock::now();
                if (delegate.closed || active_session->Finished() || (cancelled && cancelled()) ||
                    (duration > 0s && metrics.displayed > 0 && now - metrics.first_frame >= duration))
                {
                    if (modal) [NSApp stopModal];
                    else StopAppLoop();
                    return;
                }
                if (!launch.hidden && !NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion)
                {
                    const auto next = lunanx::LaunchPulseIndex(now - began);
                    if (next != pulse)
                    {
                        pulse = next;
                        for (NSInteger i = 0; i < 3; ++i)
                            progress.arrangedSubviews[i].layer.backgroundColor =
                                (i == pulse ? NSColor.whiteColor : [NSColor colorWithWhite:1 alpha:.25]).CGColor;
                    }
                }
                GCController* controller = ActiveGamepad();
                if (controller != previous_controller)
                {
                    previous_controller = controller;
                    if (controller)
                        std::cerr << "macOS gamepad: " << (controller.productCategory.UTF8String ?: "extended gamepad") << '\n';
                }
                metrics.physical_controller = metrics.physical_controller || controller != nil;
                gamepad_feed->Follow(controller);
                // A hosted layer does not follow the screen's scale: keep the
                // picture at the display's pixel density after a screen change.
                if (renderer && renderer->Layer().contentsScale != window.backingScaleFactor)
                    renderer->Layer().contentsScale = window.backingScaleFactor;
                if (exitRequested->load())
                {
                    [window close];
                    if (modal) [NSApp stopModal];
                    else StopAppLoop();
                    return;
                }
                overlay->Sample({active_session->Stats().received_bytes, metrics.displayed});
                if (overlay->Visible() && (hud.hidden || now >= nextHud))
                {
                    const auto line = overlay->Format(current_network->Label(), SystemBatteryPercent(), metrics.decoded_height);
                    hud.stringValue = [NSString stringWithFormat:@"  %@  ", [NSString stringWithUTF8String:line.c_str()]];
                    nextHud = now + 1s;
                }
                hud.hidden = !overlay->Visible();
                present();
                if (now >= metrics.next_memory)
                {
                    metrics.peak_resident = std::max(metrics.peak_resident, ResidentBytes());
                    metrics.next_memory = now + 500ms;
                }
            }
        }];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
        // ⌃⌘F or F11 switch full screen, Esc leaves it, ⌘W closes the game.
        id keys = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
            if (event.window != window)
                return event;
            if (IsFullscreenShortcut(event) || (event.keyCode == 53 && IsBorderlessFullscreen(window)))
            {
                [window toggleFullScreen:nil];
                return nil;
            }
            const auto modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
            if (event.keyCode == 13 && (modifiers & NSEventModifierFlagCommand))
            {
                [window close];
                return nil;
            }
            return event;
        }];
        const BOOL monitored_background_events = GCController.shouldMonitorBackgroundEvents;
        // A stream opened from Terminal still needs controller events while the game is visible.
        GCController.shouldMonitorBackgroundEvents = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp activateIgnoringOtherApps:YES];
            [window makeKeyAndOrderFront:nil];
        });
        session.Start();
        if (modal) [NSApp runModalForWindow:window];
        else [NSApp run];
        GCController.shouldMonitorBackgroundEvents = monitored_background_events;
        [NSEvent removeMonitor:keys];
        [timer invalidate];
        *alive = false;
        [window orderOut:nil];
        UpdatePresentation();
        const auto ended = Clock::now();
        session.Stop(!session.Finished());

        const auto counters = session.Stats();
        const double playable = metrics.displayed > 0
            ? std::chrono::duration<double>(ended - metrics.first_frame).count() : 0.0;
        const auto startup_ms = metrics.displayed > 0
            ? std::chrono::duration_cast<std::chrono::milliseconds>(metrics.first_frame - began).count() : 0;
        std::cout << std::fixed << std::setprecision(1)
                  << "Stream metrics: video_units=" << counters.video_units
                  << " decoded=" << video.DecodedFrames() << " displayed=" << metrics.displayed
                  << " playback_s=" << playable
                  << " fps=" << (playable > 0 ? metrics.displayed / playable : 0.0)
                  << " max_frame_gap_ms=" << metrics.max_frame_gap.count()
                  << " startup_ms=" << startup_ms
                  << " peak_rss_mib=" << metrics.peak_resident / (1024.0 * 1024.0)
                  << " hardware=" << (video.HardwareActive() ? "yes" : "no")
                  << " renderer=" << (renderer ? "metal" : "avfoundation")
                  << " media_bytes=" << counters.received_bytes
                  << " transport_rtt_ms=" << counters.transport_rtt_ms
                  << " transport_rtt_p95_ms=" << counters.transport_rtt_p95_ms
                  << " audio_packets=" << counters.audio_packets
                  << " audio_buffers_recycled=" << audio.PlayedBuffers()
                  << " controller_detected=" << (metrics.physical_controller ? "yes" : "no")
                  << " input_reports=" << counters.gamepad_reports
                  << " input_changes=" << counters.gamepad_changes << '\n';

        if (const auto error = session.Error(); !error.empty())
            throw std::runtime_error(error);
        if (duration > 0s && metrics.displayed == 0 && !(cancelled && cancelled()))
            throw std::runtime_error("The player closed before displaying a frame.");
    }
}

} // namespace lunaone::apple
