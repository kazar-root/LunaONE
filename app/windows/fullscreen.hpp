// SPDX-License-Identifier: GPL-3.0-only
// Full screen for the library and the player, as on the Mac: the window loses
// its frame, covers its whole monitor and the taskbar goes below it. The
// display keeps its mode.
#pragma once

#include <windows.h>
#include <shobjidl.h>

namespace lunaone::windows
{
// Without it, the taskbar can stay over a window that grew to the size of
// the monitor while it already had the focus.
inline void MarkFullscreen(HWND window, bool fullscreen)
{
    ITaskbarList2* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))))
        return;
    if (SUCCEEDED(taskbar->HrInit()))
        taskbar->MarkFullscreenWindow(window, fullscreen ? TRUE : FALSE);
    taskbar->Release();
}

// `placement` keeps the window's size and place, maximized or not, for the return.
inline void SetBorderlessFullscreen(HWND window, bool fullscreen, WINDOWPLACEMENT& placement)
{
    if (fullscreen)
    {
        // Maximize in the taskbar menu of a minimized window.
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        placement.length = sizeof(placement);
        GetWindowPlacement(window, &placement);
        MONITORINFO monitor {sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    MarkFullscreen(window, fullscreen);
}

// The maximize button, a double click on the title bar or Maximize in the
// window menu. Like the Mac's green button, they open full screen.
inline bool IsMaximizeCommand(WPARAM command)
{
    return (command & 0xfff0) == SC_MAXIMIZE;
}
} // namespace lunaone::windows
