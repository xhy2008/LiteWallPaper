#include "tray_icon.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>

namespace lwp {

namespace {

// Load the application icon embedded in the executable (resource ID 1).
HICON create_default_icon(HINSTANCE hinst) {
    return (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1),
                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
}

const wchar_t kMsgWndClass[] = L"LiteWallPaperTrayMsg";

} // namespace

TrayIcon::~TrayIcon() { destroy(); }

LRESULT CALLBACK TrayIcon::msg_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kCallbackMsg) {
            // Mouse event on the tray icon. Left or right click shows menu.
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
                if (self->cb_) self->cb_(0);
            }
            return 0;
        }
        if (msg == WM_COMMAND) {
            UINT id = LOWORD(wp);
            if (self->cb_) self->cb_(id);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool TrayIcon::create(HINSTANCE hinst, const wchar_t* tip, Callback cb) {
    hinst_ = hinst;
    cb_    = std::move(cb);
    icon_  = create_default_icon(hinst);

    // Register and create a private message-only window. This window owns
    // the notify icon and the popup menu. It is NEVER shown and NEVER becomes
    // foreground in a way that affects the wallpaper window — the wallpaper
    // window is a child of WorkerW/Progman and must stay behind the desktop
    // icons. Using the wallpaper window as the tray/menu owner would let
    // SetForegroundWindow drag it above the icon layer.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &TrayIcon::msg_wnd_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kMsgWndClass;
    RegisterClassExW(&wc);

    msg_hwnd_ = CreateWindowExW(
        0, kMsgWndClass, L"LiteWallPaperTray",
        WS_POPUP,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hinst, nullptr);
    if (!msg_hwnd_) return false;
    SetWindowLongPtrW(msg_hwnd_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd   = msg_hwnd_;
    nid_.uID    = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kCallbackMsg;
    nid_.hIcon  = icon_;
    if (tip) {
        wcsncpy_s(nid_.szTip, tip, _TRUNCATE);
    }
    return Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void TrayIcon::destroy() {
    if (nid_.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_.cbSize = 0;
    }
    if (msg_hwnd_) {
        DestroyWindow(msg_hwnd_);
        msg_hwnd_ = nullptr;
    }
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
}

void TrayIcon::set_tip(const wchar_t* tip) {
    if (tip) wcsncpy_s(nid_.szTip, tip, _TRUNCATE);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayIcon::show_menu(const std::vector<TrayMenuAction>& items) {
    HMENU menu = CreatePopupMenu();
    for (auto& it : items) {
        if (it.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else {
            UINT flags = MF_STRING;
            if (it.checked)  flags |= MF_CHECKED;
            if (it.disabled) flags |= MF_GRAYED;
            if (it.submenu)  flags |= MF_POPUP;
            AppendMenuW(menu, flags, it.id, it.label.c_str());
        }
    }

    // SetForegroundWindow on the private message window — NOT on the
    // wallpaper window. This is the Win32 popup-menu quirk fix and it must
    // not touch the wallpaper window's z-order.
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(msg_hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY,
                   pt.x, pt.y, 0, msg_hwnd_, nullptr);
    PostMessageW(msg_hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

bool TrayIcon::on_command(UINT id) {
    if (cb_) { cb_(id); return true; }
    return false;
}

} // namespace lwp
