// SPDX-License-Identifier: GPL-3.0-only
// Full screen for the library and the player, as on the Mac: the maximize
// button opens it instead of a maximized window, and Esc or F11 leave it.
#pragma once

#include <gtk/gtk.h>

namespace lunaone::linux_desktop
{
// True when the person maximized a window that already had the focus: the
// title bar button, a double click on it or a shortcut. Some window managers
// maximize a large window as it opens, or again on its way out of full
// screen. Those changes come with the window's mapping, focus or full
// screen state and do not count.
inline bool MaximizeRequested(const GdkEventWindowState* event)
{
    constexpr auto opening = GDK_WINDOW_STATE_WITHDRAWN | GDK_WINDOW_STATE_FOCUSED | GDK_WINDOW_STATE_FULLSCREEN;
    const auto state = event->new_window_state;
    return (event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) && (state & GDK_WINDOW_STATE_MAXIMIZED) &&
           (state & GDK_WINDOW_STATE_FOCUSED) && !(state & GDK_WINDOW_STATE_FULLSCREEN) &&
           !(event->changed_mask & opening);
}
} // namespace lunaone::linux_desktop
