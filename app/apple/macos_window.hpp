// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <AppKit/AppKit.h>

// The catalog and player windows. A window without a title bar cannot become
// key by default; these can, which the borderless full screen needs. The
// green button, the View menu and ⌃⌘F all reach toggleFullScreen:, which
// switches the borderless full screen instead of opening a separate Space.
@interface LunaWindow : NSWindow
// Called after each switch, to remember the choice.
@property(nonatomic, copy) void (^fullscreenChanged)(BOOL fullscreen);
@end

namespace lunaone::apple
{
// Borderless full screen: the window covers its screen without a title bar,
// the menu bar and Dock hide, and the display keeps its resolution.
void SetBorderlessFullscreen(NSWindow* window, bool fullscreen);
bool IsBorderlessFullscreen(NSWindow* window);
// Hides the menu bar and Dock while a visible window is in full screen.
void UpdatePresentation();
// ⌃⌘F, the macOS shortcut, and F11.
bool IsFullscreenShortcut(NSEvent* event);
// Size in pixels and refresh rate of the screen, for the automatic video mode.
void ReportDeviceDisplay(NSScreen* screen);
} // namespace lunaone::apple
