#pragma once
// System-tray icon with a right-click menu. Menu items:
//
//   * LiteWallPaper  (header)
//   ──────────────
//   Select wallpaper...
//   Pause / Resume
//   ──────────────
//   Settings:  Fill mode  ▶  Fill | Letterbox
//              Mute audio (no audio in v1; disabled)
//              Pause on fullscreen  ✓/✗
//   ──────────────
//   Quit
//
// Events are delivered to the App via the callback interface.
//
// IMPORTANT: TrayIcon owns a private hidden message window used as the
// notify-icon owner and popup-menu owner. This decouples tray/menu window
// management from the wallpaper window — calling SetForegroundWindow on the
// menu owner must NOT touch the wallpaper window's z-order (otherwise the
// wallpaper would jump above the desktop icons).

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <functional>

namespace lwp {

struct TrayMenuAction {
    std::wstring label;
    UINT   id;            // unique command id
    bool   checked  = false;
    bool   disabled = false;
    bool   submenu   = false;
    bool   separator = false;
};

class TrayIcon {
public:
    // id == 0 means "show the menu" (right/left click on the tray icon);
    // any other id is a WM_COMMAND id from the popup menu.
    using Callback = std::function<void(UINT id)>;

    TrayIcon() = default;
    ~TrayIcon();

    bool create(HINSTANCE hinst, const wchar_t* tip, Callback cb);
    void destroy();

    // Build and show the right-click menu at the cursor.
    void show_menu(const std::vector<TrayMenuAction>& items);

    // Handle a WM_COMMAND from the popup menu. Returns true if handled.
    bool on_command(UINT id);

    // Tooltip / status update.
    void set_tip(const wchar_t* tip);

    static constexpr UINT kCallbackMsg = WM_APP + 0x100;

private:
    static LRESULT CALLBACK msg_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp);

    HINSTANCE hinst_   = nullptr;
    HWND      msg_hwnd_ = nullptr;  // private hidden message-only window
    HICON     icon_    = nullptr;
    NOTIFYICONDATAW nid_{};
    Callback  cb_;
};

// Stable command IDs.
enum TrayCommand : UINT {
    TID_SELECT_WALLPAPER = 1001,
    TID_PAUSE_TOGGLE     = 1002,
    TID_FILL             = 1003,
    TID_LETTERBOX        = 1004,
    TID_PAUSE_ON_FS_ON   = 1005,
    TID_PAUSE_ON_FS_OFF  = 1006,
    TID_OPEN_SETTINGS    = 1007,
    TID_ABOUT            = 1008,
    TID_QUIT             = 1099,
};

} // namespace lwp
