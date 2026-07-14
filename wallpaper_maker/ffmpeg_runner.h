#pragma once
// Runs the system ffmpeg.exe to transcode any input video into a raw H.264
// Annex B byte stream with NO B-frames. The maker then post-processes that
// stream into a .lwp file (see h264_processor.h).
//
// This avoids linking FFmpeg libraries — only ffmpeg.exe on PATH is needed.

#include <string>

namespace lwp {

struct FfmpegOptions {
    // 0 means "keep source value".
    int  target_width  = 0;
    int  target_height = 0;
    int  target_fps    = 0;
    int  crf           = 23;
    std::string preset = "veryfast";
};

struct FfmpegResult {
    bool        ok = false;
    std::string error;
    // Filled from ffmpeg stderr parsing (best-effort).
    int  encoded_width  = 0;
    int  encoded_height = 0;
    int  encoded_fps    = 0;
};

// Locate ffmpeg.exe: checks FFMPEG_PATH env var, then PATH, then a few common
// install locations. Returns empty string if not found.
std::string find_ffmpeg_executable();

// Run ffmpeg to produce `output_h264` (raw Annex B) from `input`.
// The output stream is guaranteed to have:
//   - H.264 Baseline/Main/High, CRF-controlled, no B-frames
//   - repeat_headers=1 (SPS/PPS before each IDR)
//   - YUV420P, even dimensions
FfmpegResult run_ffmpeg(const std::string& input,
                        const std::string& output_h264,
                        const FfmpegOptions& opt);

} // namespace lwp
