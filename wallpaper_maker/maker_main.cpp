// wallpaper_maker — converts any ffmpeg-readable video into a .lwp file.
//
// Usage:
//   wallpaper_maker <input> <output.lwp> [options]
//
// Options:
//   -w <width>      target width (0 = keep source)
//   -h <height>     target height (0 = keep source)
//   -f <fps>        target frame rate (0 = keep source)
//   --crf <n>       x264 CRF (default 23)
//   --preset <p>    x264 preset (default veryfast)
//
// Flow:
//   1. Call ffmpeg.exe to transcode input → raw H.264 Annex B (no B-frames)
//   2. Scan the H.264 stream, split into access units, parse SPS/PPS
//   3. Write .lwp: LwpHeader + AU index table + raw Annex B payload
//
// The player then reads the AU index table and feeds each AU directly to
// D3D11 VideoDecoder — no FFmpeg dependency, no runtime NAL splitting.

#include "ffmpeg_runner.h"
#include "h264_processor.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s <input> <output.lwp> [-w W] [-h H] [-f FPS] "
        "[--crf N] [--preset P]\n", argv0);
}

int main(int argc, char** argv) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    std::string input  = argv[1];
    std::string output = argv[2];

    lwp::FfmpegOptions fopt;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int& k) -> const char* {
            if (k + 1 >= argc) return nullptr;
            return argv[++k];
        };
        if      (a == "-w" && next(i)) fopt.target_width  = std::atoi(argv[i]);
        else if (a == "-h" && next(i)) fopt.target_height = std::atoi(argv[i]);
        else if (a == "-f" && next(i)) fopt.target_fps   = std::atoi(argv[i]);
        else if (a == "--crf" && next(i)) fopt.crf = std::atoi(argv[i]);
        else if (a == "--preset" && next(i)) fopt.preset = argv[i];
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); print_usage(argv[0]); return 1; }
    }

    // --- Step 1: ffmpeg.exe -> raw H.264 ----------------------------------
    // Use a temp file next to the output.
    std::string tmp_h264 = output + ".tmp.h264";

    std::fprintf(stderr, "[maker] running ffmpeg: %s -> %s\n",
                 input.c_str(), tmp_h264.c_str());
    auto fr = lwp::run_ffmpeg(input, tmp_h264, fopt);
    if (!fr.ok) {
        std::fprintf(stderr, "[maker] ffmpeg failed: %s\n", fr.error.c_str());
        DeleteFileA(tmp_h264.c_str());
        return 2;
    }

    // --- Step 2: process H.264 -> .lwp -----------------------------------
    lwp::ProcessOptions popt;
    popt.override_fps = fopt.target_fps;

    std::fprintf(stderr, "[maker] processing: %s -> %s\n",
                 tmp_h264.c_str(), output.c_str());
    auto pr = lwp::process_h264_to_lwp(tmp_h264, output, popt);

    DeleteFileA(tmp_h264.c_str());

    if (!pr.ok) {
        std::fprintf(stderr, "[maker] FAILED: %s\n", pr.error.c_str());
        return 3;
    }
    std::fprintf(stderr,
        "[maker] OK: %u frames, %ux%u @ %u/%u fps, file=%s\n",
        pr.frame_count, pr.width, pr.height,
        pr.fps_num, pr.fps_den, output.c_str());
    return 0;
}
