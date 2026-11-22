// SPDX-License-Identifier: GPL-3.0-only

#include "macos_window.hpp"
#include "core/playback_preferences.hpp"

#import <objc/runtime.h>

#include <cmath>

@implementation LunaWindow
- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing
                              defer:(BOOL)defer
{
    if (!(self = [super initWithContentRect:rect styleMask:style backing:backing defer:defer])) return nil;
    // The green button offers full screen, which toggleFullScreen: makes borderless.
    self.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    return self;
}
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
- (void)toggleFullScreen:(id)sender
{
    const bool fullscreen = !lunaone::apple::IsBorderlessFullscreen(self);
    lunaone::apple::SetBorderlessFullscreen(self, fullscreen);
    if (self.fullscreenChanged) self.fullscreenChanged(fullscreen);
}
@end

namespace lunaone::apple
{
namespace
{
char saved_frame_key;
char saved_style_key;
} // namespace

bool IsBorderlessFullscreen(NSWindow* window)
{
    return window && objc_getAssociatedObject(window, &saved_frame_key) != nil;
}

void UpdatePresentation()
{
    bool covered = false;
    for (NSWindow* window in NSApp.windows)
        covered = covered || (window.visible && IsBorderlessFullscreen(window));
    const auto options = covered ? NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar
                                 : NSApplicationPresentationDefault;
    if (NSApp.presentationOptions != options)
        NSApp.presentationOptions = options;
}

void SetBorderlessFullscreen(NSWindow* window, bool fullscreen)
{
    if (!window || IsBorderlessFullscreen(window) == fullscreen)
        return;
    // A new style mask rebuilds the frame view and drops the keyboard focus.
    NSResponder* focus = window.firstResponder;
    if (fullscreen)
    {
        NSScreen* screen = window.screen ?: NSScreen.mainScreen;
        objc_setAssociatedObject(window, &saved_frame_key, [NSValue valueWithRect:window.frame],
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(window, &saved_style_key, @(window.styleMask), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        window.styleMask = NSWindowStyleMaskBorderless;
        [window setFrame:screen.frame display:YES];
    }
    else
    {
        const NSRect frame = [objc_getAssociatedObject(window, &saved_frame_key) rectValue];
        NSNumber* style = objc_getAssociatedObject(window, &saved_style_key);
        objc_setAssociatedObject(window, &saved_frame_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(window, &saved_style_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        window.styleMask = style.unsignedIntegerValue;
        [window setFrame:frame display:YES];
    }
    [window makeKeyAndOrderFront:nil];
    if (focus && focus != window)
        [window makeFirstResponder:focus];
    UpdatePresentation();
}

bool IsFullscreenShortcut(NSEvent* event)
{
    constexpr NSEventModifierFlags chord = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    const auto modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
                           ~(NSEventModifierFlagFunction | NSEventModifierFlagNumericPad | NSEventModifierFlagCapsLock);
    return (event.keyCode == 3 && modifiers == chord) || (event.keyCode == 103 && !(modifiers & ~NSEventModifierFlagFunction));
}

void ReportDeviceDisplay(NSScreen* screen)
{
    screen = screen ?: NSScreen.mainScreen;
    if (!screen)
        return;
    int refresh = 0;
    if (@available(macOS 12.0, *))
        refresh = static_cast<int>(screen.maximumFramesPerSecond);
    lunanx::SetDeviceDisplay({static_cast<int>(std::lround(screen.frame.size.height * screen.backingScaleFactor)), refresh});
}
} // namespace lunaone::apple
