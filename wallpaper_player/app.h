#pragma once
// App — top-level orchestrator. Owns the window, renderer, decoder, tray icon,
// config and monitor; drives the playback loop on a Win32 timer.

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#include "wallpaper_window.h"
#include "d3d11_renderer.h"
#include "d3d11_decoder.h"
#include "tray_icon.h"
#include "config_manager.h"
#include "foreground_monitor.h"
#include "nal_parser.h"
#include <bitstream_format.h>

namespace lwp {

class App {
public:
    App() = default;
    ~App();

    bool init(HINSTANCE hinst);
    void run();              // message loop
    void quit();

    // Accessor used by player_main to install its message hook.
    WallpaperWindow& window() { return window_; }

    // Window-message hook used by player_main.
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    // --- playback ---
    bool load_wallpaper(const std::wstring& path);
    void close_wallpaper();
    void on_frame_tick();    // called by timer; decodes & renders one frame
    bool decode_next_frame();

    // --- menu ---
    void show_tray_menu();
    void on_tray_command(UINT id);
    void on_select_wallpaper();

    // --- lifecycle helpers ---
    void set_paused(bool p);
    void update_tip();

    HINSTANCE           hinst_ = nullptr;
    WallpaperWindow     window_;
    D3D11Renderer       renderer_;
    D3D11VideoDecoder   decoder_;
    TrayIcon            tray_;
    ConfigManager       config_;
    ForegroundMonitor   monitor_;

    // Loaded .lwp data. Only the header, AU index table, SPS and PPS are kept
    // in memory; the raw H.264 payload is read frame-by-frame from the file
    // via `file_handle_` to keep memory footprint small (a 33MB wallpaper
    // would otherwise stay resident in payload_).
    LwpHeader           header_{};
    SpsParams           sps_{};
    std::vector<uint8_t> sps_raw_;   // SPS NAL bytes (relative to payload)
    std::vector<uint8_t> pps_raw_;   // PPS NAL bytes (relative to payload)

    // AU index table read from the .lwp file (pre-built by the maker).
    // Each entry gives the offset/size of one access unit within the payload.
    std::vector<AuEntry> au_table_;
    uint32_t             au_index_ = 0;

    // File handle kept open for streaming. `payload_base_` is the file-absolute
    // offset where the H.264 payload starts (right after the AU index table).
    HANDLE   file_handle_ = nullptr;
    uint64_t payload_base_ = 0;
    // Single-frame read buffer, reused across frames to avoid reallocation.
    std::vector<uint8_t> au_buffer_;

    // Timing.
    uint32_t frame_ms_ = 33;              // ms per frame (1000 / fps)
    uint64_t next_frame_time_ = 0;        // GetTickCount64 at next due tick
    bool     paused_ = false;             // user-requested pause
    bool     blocking_ = false;           // foreground app is maximized/FS

    UINT_PTR timer_id_ = 0;
    bool     quitting_ = false;
};

} // namespace lwp
