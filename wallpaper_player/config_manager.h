#pragma once
// Tiny persistent config store: a single INI file next to the executable.
// No registry, no JSON parser, no third-party deps.

#include <string>
#include <windows.h>

namespace lwp {

struct Settings {
    std::wstring wallpaper_path;          // path to current .lwp file
    bool         pause_on_fullscreen = true;
    bool         fill_mode           = true;  // true = stretch, false = letterbox
    bool         muted               = true;  // no audio path yet; always true
    int          target_fps          = 0;     // 0 = use file's native fps
};

class ConfigManager {
public:
    bool load();
    bool save() const;

    const Settings& settings() const { return s_; }
    Settings&       settings()       { return s_; }

    // Path to the INI (next to the exe).
    std::wstring ini_path() const;

private:
    Settings s_;
};

} // namespace lwp
