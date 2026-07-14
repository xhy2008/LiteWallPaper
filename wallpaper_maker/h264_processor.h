#pragma once
// Post-processes a raw H.264 Annex B file (produced by ffmpeg_runner) into a
// .lwp file. Responsibilities:
//
//   1. Scan the stream for SPS (NAL type 7) and PPS (NAL type 8).
//   2. Parse the SPS for profile/level/bit-depth/coded dimensions.
//   3. Split the stream into access units (one AU == one picture, because
//      there are no B-frames and one slice per picture).
//   4. Write: LwpHeader + AU index table + raw Annex B payload.

#include <string>
#include <cstdint>

namespace lwp {

struct ProcessOptions {
    // Overrides the fps from SPS/stream if > 0. ffmpeg already bakes the
    // frame rate into the stream, so this is usually 0.
    int override_fps = 0;
};

struct ProcessResult {
    bool        ok = false;
    std::string error;
    uint32_t    frame_count = 0;
    uint16_t    width       = 0;
    uint16_t    height      = 0;
    uint16_t    fps_num     = 0;
    uint16_t    fps_den     = 1;
};

// Read `h264_path` (raw Annex B), write `lwp_path`.
ProcessResult process_h264_to_lwp(const std::string& h264_path,
                                  const std::string& lwp_path,
                                  const ProcessOptions& opt);

} // namespace lwp
