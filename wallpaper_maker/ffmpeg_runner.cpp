#include "ffmpeg_runner.h"

#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

namespace lwp {

namespace {

// Search PATH for an executable. Returns full path or empty.
std::string search_path(const char* exe) {
    char buf[MAX_PATH];
    DWORD len = SearchPathA(nullptr, exe, ".exe", MAX_PATH, buf, nullptr);
    if (len > 0 && len < MAX_PATH) return std::string(buf);
    return "";
}

} // namespace

std::string find_ffmpeg_executable() {
    // 1. FFMPEG_PATH env var (can be full path or directory).
    char env[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("FFMPEG_PATH", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string s(env);
        // If it's a directory, append ffmpeg.exe.
        DWORD attr = GetFileAttributesA(s.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            s += "\\ffmpeg.exe";
        }
        if (GetFileAttributesA(s.c_str()) != INVALID_FILE_ATTRIBUTES)
            return s;
    }

    // 2. PATH.
    std::string p = search_path("ffmpeg");
    if (!p.empty()) return p;

    // 3. Common install locations.
    const char* candidates[] = {
        "C:\\ffmpeg\\bin\\ffmpeg.exe",
        "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
        "C:\\Program Files (x86)\\ffmpeg\\bin\\ffmpeg.exe",
    };
    for (auto* c : candidates) {
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return "";
}

FfmpegResult run_ffmpeg(const std::string& input,
                        const std::string& output_h264,
                        const FfmpegOptions& opt) {
    FfmpegResult r;

    std::string exe = find_ffmpeg_executable();
    if (exe.empty()) {
        r.error = "ffmpeg.exe not found. Install FFmpeg or set FFMPEG_PATH.";
        return r;
    }

    // Build command line. Key flags:
    //   -an                     drop audio
    //   -c:v libx264            H.264 encoder
    //   -profile:v baseline     Baseline profile (CAVLC, no transform_8x8)
    //                           — maximum DXVA compatibility
    //   -x264-params bframes=0:repeat_headers=1
    //                           no B-frames, SPS/PPS before each IDR
    //   -pix_fmt yuv420p        8-bit 4:2:0 (DXVA-friendly)
    //   -f h264                 raw Annex B output
    //   -vf scale=W:H           resize (if specified)
    //   -r FPS                  frame rate (if specified)
    //   -y                      overwrite output
    std::string cmd = "\"" + exe + "\" -y -i \"" + input + "\" -an";
    cmd += " -c:v libx264";
    cmd += " -profile:v baseline";
    cmd += " -preset " + opt.preset;
    char crf[16]; std::snprintf(crf, sizeof(crf), "%d", opt.crf);
    cmd += " -crf " + std::string(crf);
    cmd += " -x264-params bframes=0:repeat_headers=1";
    cmd += " -pix_fmt yuv420p";

    if (opt.target_width > 0 && opt.target_height > 0) {
        // Ensure even dimensions (yuv420p requirement).
        int w = (opt.target_width  / 2) * 2;
        int h = (opt.target_height / 2) * 2;
        char vf[64]; std::snprintf(vf, sizeof(vf), "scale=%d:%d", w, h);
        cmd += " -vf " + std::string(vf);
    } else if (opt.target_width > 0) {
        int w = (opt.target_width / 2) * 2;
        char vf[64]; std::snprintf(vf, sizeof(vf), "scale=%d:-2", w);
        cmd += " -vf " + std::string(vf);
    } else if (opt.target_height > 0) {
        int h = (opt.target_height / 2) * 2;
        char vf[64]; std::snprintf(vf, sizeof(vf), "scale=-2:%d", h);
        cmd += " -vf " + std::string(vf);
    }

    if (opt.target_fps > 0) {
        char fps[16]; std::snprintf(fps, sizeof(fps), "%d", opt.target_fps);
        cmd += " -r " + std::string(fps);
    }

    cmd += " -f h264 \"" + output_h264 + "\"";
    cmd += " 2>&1"; // merge stderr so we can capture diagnostics

    // _popen runs "cmd.exe /c <cmd>". When <cmd> has more than two quote
    // characters, cmd.exe strips the first and last quote — corrupting the
    // exe path and filenames. Wrap the entire command in an extra pair of
    // outer quotes so cmd.exe strips only those, leaving inner quotes intact.
    cmd = "\"" + cmd + "\"";

    // Run via popen to capture output.
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        r.error = "failed to launch ffmpeg";
        return r;
    }

    std::string log;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        log += buf;
    }
    int exit_code = _pclose(pipe);

    if (exit_code != 0) {
        r.error = "ffmpeg exited with code " + std::to_string(exit_code);
        // Include last ~500 chars of log for diagnostics.
        if (log.size() > 500) log = log.substr(log.size() - 500);
        r.error += "\n" + log;
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace lwp
