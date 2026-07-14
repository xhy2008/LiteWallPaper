#include "config_manager.h"

#include <shlwapi.h>
#include <vector>

namespace lwp {

namespace {

std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring read_ini(const wchar_t* file, const wchar_t* section,
                      const wchar_t* key, const wchar_t* def) {
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(section, key, def ? def : L"",
                             buf, MAX_PATH, file);
    return buf;
}

void write_ini(const wchar_t* file, const wchar_t* section,
               const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, file);
}

void write_ini_int(const wchar_t* file, const wchar_t* section,
                   const wchar_t* key, int v) {
    wchar_t buf[16]; swprintf_s(buf, L"%d", v);
    write_ini(file, section, key, buf);
}

} // namespace

std::wstring ConfigManager::ini_path() const {
    return exe_dir() + L"\\LiteWallPaper.ini";
}

bool ConfigManager::load() {
    auto p = ini_path();
    s_.wallpaper_path       = read_ini(p.c_str(), L"main", L"wallpaper", L"");
    s_.pause_on_fullscreen  = GetPrivateProfileIntW(L"main", L"pause_on_fs", 1, p.c_str()) != 0;
    s_.fill_mode            = GetPrivateProfileIntW(L"main", L"fill", 1, p.c_str()) != 0;
    s_.muted                = GetPrivateProfileIntW(L"main", L"muted", 1, p.c_str()) != 0;
    s_.target_fps           = GetPrivateProfileIntW(L"main", L"target_fps", 0, p.c_str());
    return true;
}

bool ConfigManager::save() const {
    auto p = ini_path();
    write_ini(p.c_str(), L"main", L"wallpaper", s_.wallpaper_path.c_str());
    write_ini_int(p.c_str(), L"main", L"pause_on_fs", s_.pause_on_fullscreen ? 1 : 0);
    write_ini_int(p.c_str(), L"main", L"fill",        s_.fill_mode ? 1 : 0);
    write_ini_int(p.c_str(), L"main", L"muted",       s_.muted ? 1 : 0);
    write_ini_int(p.c_str(), L"main", L"target_fps",  s_.target_fps);
    return true;
}

} // namespace lwp
