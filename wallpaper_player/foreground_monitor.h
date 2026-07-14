#pragma once
// Polls the foreground window and notifies when any foreground window is
// maximized or fullscreen on the primary monitor. We use this to pause the
// wallpaper playback (saving CPU/GPU) while the user is gaming or watching
// a video in fullscreen.
//
// Why poll instead of event hooks: SetWinEventHook works but is heavier and
// fires for every window movement. A 1 Hz poll is ~0% CPU and plenty fast.

#include <windows.h>
#include <functional>

namespace lwp {

class ForegroundMonitor {
public:
    using StateChanged = std::function<void(bool any_maximized_or_fs)>;

    ForegroundMonitor() = default;
    explicit ForegroundMonitor(StateChanged cb) : cb_(std::move(cb)) {}

    void set_callback(StateChanged cb) { cb_ = std::move(cb); }

    // Call once per second from the main thread. Cheap: a few hundred
    // microseconds of GetForegroundWindow + IsZoomed + monitor bounds.
    void poll();

    bool current_state() const { return blocking_; }

private:
    StateChanged cb_;
    bool blocking_ = false;
};

} // namespace lwp
