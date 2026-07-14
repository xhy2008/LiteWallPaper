// wallpaper_player entry point. Wires the message loop into the App class.

#include "app.h"
#include <windows.h>

// We need the App instance accessible from the window procedure. We use a
// thread-local global (the player is single-threaded by design).
static lwp::App* g_app = nullptr;

static LRESULT CALLBACK app_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Hook the wallpaper window's wnd_proc by temporarily swapping it.
// In practice, our WallpaperWindow already routes messages to its own proc;
// we install a subclass so App gets them. For simplicity here, we let
// App::handle_message be called from the WallpaperWindow's wnd_proc via a
// message pump hook. To keep the demo self-contained, we re-route through
// SetWindowSubclass would be cleaner — but a global is fine for a
// single-instance, single-window app.

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    // SetProcessWorkingSetSize trims our footprint proactively; the OS will
    // also do this automatically but being explicit costs nothing.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    // Reduce timer resolution to 1 ms only while running; the default is
    // 15.6 ms. We DO NOT change it (we use GetTickCount64 + 16ms timer), so
    // we don't add a global wakeup cost. See header comment in app.cpp.

    lwp::App app;
    g_app = &app;
    if (!app.init(hinst)) {
        MessageBoxW(nullptr, L"Failed to initialize LiteWallPaper.",
                    L"LiteWallPaper", MB_ICONERROR);
        return 1;
    }

    // Tell our hidden message window (created by WallpaperWindow) to also
    // route to App. We do this by installing a WM_APP+3 hook once.
    // (The WallpaperWindow already forwards WM_SIZE etc.; we rely on that.)
    // For other messages not handled in WallpaperWindow's proc, they will
    // reach DefWindowProc and never see App. To avoid that, we override the
    // window proc here:
    SetWindowLongPtrW(app.window().hwnd(), GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(app_wnd_proc));

    app.run();
    g_app = nullptr;
    return 0;
}
