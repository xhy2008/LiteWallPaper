#include "wallpaper_window.h"

#include <shellapi.h>
#include <cstring>
#include <vector>

namespace lwp {

// EnumWindows callback to locate the WorkerW that hosts SHELLDLL_DefView.
// After sending 0x052C to Progman, Explorer moves SHELLDLL_DefView into a
// sibling WorkerW. We want THAT WorkerW's sibling (the one below it in
// z-order without SHELLDLL_DefView) — that's the wallpaper layer.
// If 0x052C didn't move SHELLDLL_DefView (it stays inside Progman), then
// we want the WorkerW that is a sibling of Progman and sits below it.
struct FindWorkerData {
    HWND progman;
    HWND shelldll_worker; // WorkerW that contains SHELLDLL_DefView (if any)
    HWND wallpaper_worker; // a WorkerW sibling of Progman, below it
};

BOOL CALLBACK find_worker_enum_cb(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindWorkerData*>(lp);
    wchar_t cls[32] = {};
    if (!GetClassNameW(hwnd, cls, 32)) return TRUE;
    if (wcscmp(cls, L"WorkerW") != 0) return TRUE;

    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
        // This WorkerW hosts the desktop icons.
        if (!d->shelldll_worker) d->shelldll_worker = hwnd;
    } else {
        // A WorkerW without icons. Keep the first one found (topmost).
        if (!d->wallpaper_worker) d->wallpaper_worker = hwnd;
    }
    return TRUE;
}

HWND find_worker_w() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // Step 1: check if SHELLDLL_DefView is currently inside Progman. If so,
    // send 0x052C to make Explorer move it into a WorkerW (this also spawns
    // the wallpaper-layer WorkerW as a sibling below Progman).
    HWND shell_in_progman = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);

    if (shell_in_progman) {
        // Spawn the wallpaper WorkerW. Retry up to 5x: Explorer occasionally
        // needs a moment to create it.
        for (int i = 0; i < 5; ++i) {
            SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 2000, nullptr);
            Sleep(100);
            // After 0x052C, SHELLDLL_DefView should move OUT of Progman.
            if (!FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
                break;
            }
        }
    }

    // Step 2: enumerate ALL WorkerW windows (EnumWindows skips invisible
    // top-level windows on some systems, so use FindWindowEx which finds
    // them regardless of visibility). Classify each:
    //   - "icon worker"   = has SHELLDLL_DefView child (the desktop icons)
    //   - "wallpaper worker" = no SHELLDLL_DefView child (candidate layer)
    // Among wallpaper workers, prefer one that:
    //   - is visible (the real wallpaper layer is visible)
    //   - has a non-trivial size (170x47 are icon-label tooltips)
    HWND icon_worker = nullptr;
    HWND wallpaper_visible = nullptr;
    HWND wallpaper_any = nullptr;
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr))) {
        bool has_shell = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
        bool vis = IsWindowVisible(hwnd) != 0;
        RECT rc{}; GetWindowRect(hwnd, &rc);
        LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (has_shell) {
            if (!icon_worker) icon_worker = hwnd;
        } else {
            // Skip tiny tooltip-sized WorkerW windows (icon labels are ~170x47).
            bool sized = (w >= 640 && h >= 480);
            if (vis && sized && !wallpaper_visible) wallpaper_visible = hwnd;
            if (!wallpaper_any) wallpaper_any = hwnd;
        }
    }

    if (wallpaper_visible) {
        return wallpaper_visible;
    }

    // Step 3: no suitable wallpaper WorkerW found. The desktop on this system
    // does not expose a wallpaper layer WorkerW. Fall back to Progman itself:
    // create() will SetParent into Progman and position our window behind
    // SHELLDLL_DefView so icons stay on top.
    return progman;
}

WallpaperWindow::~WallpaperWindow() { destroy(); }

bool WallpaperWindow::create(HINSTANCE hinst, const wchar_t* title) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &WallpaperWindow::wnd_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = L"LiteWallPaperWindow";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    // Create the window hidden at first; we position it after parenting.
    // NOTE: do NOT use WS_EX_LAYERED — a layered window needs
    // SetLayeredWindowAttributes/UpdateLayeredWindow to become visible, and
    // D3D swap chains present into a surface the DWM compositor ignores for
    // layered windows. The WorkerW SetParent trick alone puts us behind the
    // desktop icons; no layered style is needed.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, title ? title : L"LiteWallPaper",
        WS_POPUP | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    // Parent onto WorkerW so we render behind desktop icons.
    worker_w_ = find_worker_w();
    if (worker_w_) {
        SetParent(hwnd_, worker_w_);

        // If we fell back to Progman (no wallpaper WorkerW found), our
        // window is now a child of Progman alongside SHELLDLL_DefView. We
        // must sit BEHIND SHELLDLL_DefView so desktop icons show on top.
        // SetWindowPos with hwndInsertAfter = the SHELLDLL_DefView HWND
        // places us directly below it in the sibling z-order.
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (worker_w_ == progman) {
            // SHELLDLL_DefView may be a direct child of Progman, or nested
            // deeper. Search Progman's descendants.
            HWND shell = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
            if (shell) {
                SetWindowPos(hwnd_, shell, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                // SHELLDLL_DefView not found inside Progman (it was moved to
                // a WorkerW by 0x052C). Just send us to the bottom.
                SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }

    fit_to_desktop();

    return true;
}

void WallpaperWindow::destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    worker_w_ = nullptr;
}

void WallpaperWindow::fit_to_desktop() {
    if (!hwnd_) return;
    // Virtual screen covers all monitors; we use the primary monitor for the
    // simple case. A multi-monitor stretch could be added later.
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        x = y = 0;
    }
    SetWindowPos(hwnd_, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

void WallpaperWindow::on_display_changed() {
    fit_to_desktop();
    // The renderer will pick up the new size via WM_SIZE.
}

LRESULT CALLBACK WallpaperWindow::wnd_proc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp) {
    WallpaperWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = static_cast<WallpaperWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<WallpaperWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        switch (msg) {
        case WM_SIZE:
            // Forward to the app via a custom message handled in main.
            PostMessageW(hwnd, WM_APP + 2, wp, lp);
            return 0;
        case WM_DISPLAYCHANGE:
            self->on_display_changed();
            return 0;
        case WM_ERASEBKGND:
            return 1; // D3D clears the backbuffer; never GDI-fill.
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace lwp
