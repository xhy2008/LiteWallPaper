#include "app.h"

#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>

namespace lwp {

App::~App() { quit(); }

bool App::init(HINSTANCE hinst) {
    hinst_ = hinst;
    if (!config_.load()) return false;

    if (!window_.create(hinst, L"LiteWallPaper")) return false;

    // Tray icon — uses its own private message window as owner so that
    // SetForegroundWindow during menu display never touches the wallpaper
    // window's z-order.
    if (!tray_.create(hinst, L"LiteWallPaper",
                      [this](UINT id){
                          if (id == 0) show_tray_menu();
                          else          on_tray_command(id);
                      })) {
        return false;
    }

    monitor_.set_callback([this](bool b){
        blocking_ = b;
        update_tip();
    });

    // Load the saved wallpaper if any.
    if (!config_.settings().wallpaper_path.empty()) {
        load_wallpaper(config_.settings().wallpaper_path);
    }

    // Frame timer: 60 Hz poll, we throttle actual decode to the file's fps.
    timer_id_ = SetTimer(window_.hwnd(), 1, 16, nullptr);
    return true;
}

void App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void App::quit() {
    if (quitting_) return;
    quitting_ = true;
    if (timer_id_) { KillTimer(window_.hwnd(), timer_id_); timer_id_ = 0; }
    close_wallpaper();
    tray_.destroy();
    renderer_.shutdown();
    window_.destroy();
    PostQuitMessage(0);
}

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) {
            on_frame_tick();
            // 1 Hz foreground poll.
            static DWORD last_poll = 0;
            DWORD now = GetTickCount();
            if (now - last_poll >= 1000) {
                if (config_.settings().pause_on_fullscreen) {
                    monitor_.poll();
                }
                last_poll = now;
            }
        }
        return 0;
    case WM_APP + 2: { // WM_SIZE forwarded from WallpaperWindow
        if (renderer_.resize(LOWORD(lp), HIWORD(lp))) {}
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        window_.on_display_changed();
        RECT rc; GetClientRect(hwnd, &rc);
        renderer_.resize((uint32_t)(rc.right - rc.left),
                         (uint32_t)(rc.bottom - rc.top));
        return 0;
    }
    case WM_SIZE: {
        renderer_.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

bool App::load_wallpaper(const std::wstring& path) {
    close_wallpaper();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fsz; GetFileSizeEx(h, &fsz);
    if (fsz.QuadPart < (LONGLONG)sizeof(LwpHeader)) { CloseHandle(h); return false; }

    DWORD got;
    if (!ReadFile(h, &header_, sizeof(header_), &got, nullptr) ||
        got != sizeof(header_) || header_.magic != kMagic) {
        CloseHandle(h); return false;
    }

    // Read AU index table (pre-built by the maker — no runtime NAL splitting).
    au_table_.resize(header_.frame_count);
    LARGE_INTEGER au_pos;
    au_pos.QuadPart = header_.au_table_offset;
    SetFilePointerEx(h, au_pos, nullptr, FILE_BEGIN);
    DWORD au_bytes = header_.frame_count * header_.au_entry_size;
    if (au_bytes != header_.frame_count * sizeof(AuEntry)) {
        CloseHandle(h); return false; // entry size mismatch
    }
    if (!ReadFile(h, au_table_.data(), au_bytes, &got, nullptr) ||
        got != au_bytes) {
        CloseHandle(h); au_table_.clear(); return false;
    }

    // Payload starts right after the AU index table. Keep the file handle
    // open and read frames on demand instead of loading the whole payload
    // into memory.
    payload_base_ = (uint64_t)header_.au_table_offset + au_bytes;

    // Read SPS and PPS NALs (small — just a few dozen bytes each).
    sps_raw_.clear();
    pps_raw_.clear();
    auto read_nal = [&](uint32_t off, uint16_t size, std::vector<uint8_t>& out) -> bool {
        if (size == 0) return true;
        out.resize(size);
        LARGE_INTEGER pos; pos.QuadPart = payload_base_ + off;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
        DWORD r = 0;
        if (!ReadFile(h, out.data(), size, &r, nullptr) || r != size) return false;
        return true;
    };
    if (!read_nal(header_.sps_offset, header_.sps_size, sps_raw_) ||
        !read_nal(header_.pps_offset, header_.pps_size, pps_raw_)) {
        CloseHandle(h); au_table_.clear(); return false;
    }

    // Parse SPS.
    if (!sps_raw_.empty()) {
        sps_ = parse_sps(sps_raw_.data(), sps_raw_.size());
    }
    if (!sps_.ok) { CloseHandle(h); return false; }

    // Init decoder + renderer.
    DecoderConfig dcfg;
    dcfg.sps = sps_;
    if (!pps_raw_.empty()) {
        dcfg.pps = pps_raw_;
        dcfg.pps_parsed = parse_pps(dcfg.pps.data(), dcfg.pps.size(),
                                     sps_.profile_idc);
    }

    // Keep the file handle open for streaming reads in decode_next_frame().
    file_handle_ = h;

    // Renderer first so we can pass its device to the decoder.
    // Only reinitialize the renderer if it's not yet initialized or the
    // video dimensions changed; otherwise reuse the existing device/swapchain.
    bool need_ren_init = !renderer_.initialized() ||
                         renderer_.video_width()  != sps_.coded_width ||
                         renderer_.video_height() != sps_.coded_height;
    if (need_ren_init) {
        renderer_.shutdown();
        if (!renderer_.initialize(window_.hwnd(), sps_.coded_width, sps_.coded_height)) {
            return false;
        }
    }
    if (!decoder_.initialize(renderer_.device(), dcfg)) {
        return false;
    }

    // Frame timing.
    if (header_.fps_num > 0 && header_.fps_den > 0) {
        frame_ms_ = (uint32_t)((uint64_t)1000 * header_.fps_den / header_.fps_num);
    } else {
        frame_ms_ = 33;
    }
    if (config_.settings().target_fps > 0) {
        frame_ms_ = (uint32_t)(1000 / config_.settings().target_fps);
    }
    next_frame_time_ = GetTickCount64();

    // Persist selection.
    config_.settings().wallpaper_path = path;
    config_.save();
    update_tip();

    // Render the first frame immediately so the wallpaper shows right away
    // without waiting for the next timer tick. Also nudge the window to
    // repaint — flip-discard swapchains sometimes need a WM_PAINT to push
    // the first frame through the DWM compositor.
    if (!paused_ && !blocking_) {
        decode_next_frame();
    }
    InvalidateRect(window_.hwnd(), nullptr, FALSE);
    return true;
}

void App::close_wallpaper() {
    decoder_.shutdown();
    if (file_handle_) { CloseHandle(file_handle_); file_handle_ = nullptr; }
    // Free the actual buffer memory, not just reset the size. swap() with an
    // empty vector releases the underlying allocation immediately.
    std::vector<uint8_t>().swap(sps_raw_);
    std::vector<uint8_t>().swap(pps_raw_);
    std::vector<uint8_t>().swap(au_buffer_);
    std::vector<AuEntry>().swap(au_table_);
    au_index_ = 0;
    payload_base_ = 0;
    sps_ = {};
}

bool App::decode_next_frame() {
    if (au_table_.empty() || !file_handle_) return false;
    if (au_index_ >= au_table_.size()) au_index_ = 0; // loop

    const AuEntry& au = au_table_[au_index_];
    // Read this single AU from the file. The buffer is reused across frames
    // so it only grows as large as the biggest frame, not the whole payload.
    if (au_buffer_.size() < au.size) au_buffer_.resize(au.size);
    LARGE_INTEGER pos; pos.QuadPart = payload_base_ + au.offset;
    DWORD got = 0;
    if (!SetFilePointerEx(file_handle_, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(file_handle_, au_buffer_.data(), au.size, &got, nullptr) ||
        got != au.size) {
        ++au_index_;
        return false;
    }

    ComPtr<ID3D11Texture2D> out;
    bool ok = decoder_.decode(au_buffer_.data(), au.size, &out);
    if (!ok) {
        ++au_index_;
        return false;
    }
    ++au_index_;

    renderer_.render(out.Get(), config_.settings().fill_mode);
    renderer_.present();
    return true;
}

void App::on_frame_tick() {
    if (paused_ || blocking_) return;
    if (au_table_.empty() || !file_handle_) return;

    uint64_t now = GetTickCount64();
    // Catch up if we fell behind, but never more than 2 frames per tick.
    int budget = 2;
    while (now >= next_frame_time_ && budget-- > 0) {
        if (!decode_next_frame()) break;
        next_frame_time_ += frame_ms_;
    }
    // If we somehow drifted far ahead (e.g., after a long pause), resync.
    if (next_frame_time_ < now) next_frame_time_ = now + frame_ms_;
}

void App::set_paused(bool p) {
    paused_ = p;
    update_tip();
}

void App::update_tip() {
    wchar_t buf[128];
    const wchar_t* state = blocking_ ? L" (paused: app FS)"
                       : (paused_   ? L" (paused)"
                                    : L"");
    swprintf_s(buf, L"LiteWallPaper%s", state);
    tray_.set_tip(buf);
}

// --- Menu ------------------------------------------------------------------

void App::show_tray_menu() {
    std::vector<TrayMenuAction> items;

    // Title (disabled).
    items.push_back({ L"LiteWallPaper", 0, false, true });

    items.push_back({ L"Select wallpaper...", TID_SELECT_WALLPAPER });
    items.push_back({ paused_ ? L"Resume" : L"Pause", TID_PAUSE_TOGGLE });
    items.push_back({ L"", 0, false, false, false, true });

    // Fill mode: two checkable items.
    items.push_back({ config_.settings().fill_mode ? L"\u2713 Fill" : L"  Fill",
                      TID_FILL });
    items.push_back({ !config_.settings().fill_mode ? L"\u2713 Letterbox" : L"  Letterbox",
                      TID_LETTERBOX });
    items.push_back({ L"", 0, false, false, false, true });

    items.push_back({ config_.settings().pause_on_fullscreen
                       ? L"\u2713 Pause on fullscreen app"
                       : L"  Pause on fullscreen app",
                      TID_PAUSE_ON_FS_ON });
    items.push_back({ L"", 0, false, false, false, true });
    items.push_back({ L"Quit", TID_QUIT });

    tray_.show_menu(items);
}

void App::on_tray_command(UINT id) {
    switch (id) {
    case TID_SELECT_WALLPAPER:
        on_select_wallpaper();
        break;
    case TID_PAUSE_TOGGLE:
        set_paused(!paused_);
        break;
    case TID_FILL:
        config_.settings().fill_mode = true; config_.save(); break;
    case TID_LETTERBOX:
        config_.settings().fill_mode = false; config_.save(); break;
    case TID_PAUSE_ON_FS_ON:
        config_.settings().pause_on_fullscreen = !config_.settings().pause_on_fullscreen;
        config_.save();
        if (!config_.settings().pause_on_fullscreen) blocking_ = false;
        update_tip();
        break;
    case TID_QUIT:
        quit();
        break;
    }
}

void App::on_select_wallpaper() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    // Do NOT use the wallpaper window as the dialog owner: GetOpenFileNameW
    // activates its owner, which would raise the wallpaper window above the
    // desktop icons. Use nullptr so the dialog is standalone.
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = L"LiteWallPaper files (*.lwp)\0*.lwp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        if (!load_wallpaper(file)) {
            // Use nullptr owner for the same z-order reason as the dialog.
            MessageBoxW(nullptr,
                L"Failed to load the wallpaper. The file may be corrupt or "
                L"the GPU does not support H.264 DXVA decoding.",
                L"LiteWallPaper", MB_ICONWARNING);
        }
    }
}

} // namespace lwp
