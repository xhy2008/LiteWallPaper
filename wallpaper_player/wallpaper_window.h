#pragma once
// Creates a top-level window that sits behind all desktop icons, on top of
// the desktop wallpaper proper. This is the "behind the icons" trick used by
// Wallpaper Engine and similar tools:
//
//   1. Send a SPI_SETDESKWALLPAPER message to Program Manager (Progman) with
//      an empty PNG to force it to spawn a WorkerW child window.
//   2. Find that WorkerW by enumerating sibling windows of Progman.
//   3. Reparent our window (SetParent) onto WorkerW.
//
// The result: our window is rendered behind all desktop icons, in front of
// the static wallpaper color, and the DWM compositor keeps it on the desktop
// layer (not the application layer), so it costs almost nothing when the user
// is in a fullscreen app on top.

#include <windows.h>
#include <string>

namespace lwp {

class WallpaperWindow {
public:
    WallpaperWindow() = default;
    ~WallpaperWindow();

    bool create(HINSTANCE hinst, const wchar_t* title);
    void destroy();

    HWND hwnd() const { return hwnd_; }

    // Reposition to cover the primary monitor's virtual screen.
    void fit_to_desktop();

    // Notify on WM_DISPLAYCHANGE / monitor reconfig.
    void on_display_changed();

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    HWND     hwnd_     = nullptr;
    HWND     worker_w_ = nullptr;
    HINSTANCE hinst_   = nullptr;
};

// Helper: locate the WorkerW window that hosts the desktop wallpaper layer.
HWND find_worker_w();

} // namespace lwp
