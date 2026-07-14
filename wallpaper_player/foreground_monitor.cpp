#include "foreground_monitor.h"

#include <cstring>

namespace lwp {

namespace {

bool is_fullscreen_window(HWND hwnd) {
    if (!hwnd) return false;
    RECT wr; if (!GetWindowRect(hwnd, &wr)) return false;

    // Check the monitor that the window is mostly on.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    // Fullscreen if the window covers the entire monitor's work area / screen.
    return wr.left   <= mi.rcMonitor.left
        && wr.top    <= mi.rcMonitor.top
        && wr.right  >= mi.rcMonitor.right
        && wr.bottom >= mi.rcMonitor.bottom;
}

} // namespace

void ForegroundMonitor::poll() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;

    // Skip our own wallpaper window (it's not a real foreground target
    // because of WS_EX_NOACTIVATE, but be defensive).
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (std::wcscmp(cls, L"LiteWallPaperWindow") == 0) return;

    bool blocking = IsZoomed(fg) || is_fullscreen_window(fg);
    // Exclude the desktop / shell windows from triggering pause (they are
    // maximized by default).
    if (std::wcscmp(cls, L"Progman") == 0 ||
        std::wcscmp(cls, L"WorkerW") == 0) {
        blocking = false;
    }

    if (blocking != blocking_) {
        blocking_ = blocking;
        if (cb_) cb_(blocking_);
    }
}

} // namespace lwp
