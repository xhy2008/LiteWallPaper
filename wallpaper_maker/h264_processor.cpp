#include "h264_processor.h"
#include "sps_parser.h"

#include <bitstream_format.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace lwp {

namespace {

// Find start code at position i. Returns start-code length (3 or 4) or 0.
inline size_t start_code_len(const uint8_t* p, size_t n, size_t i) {
    if (i + 4 <= n && p[i]==0 && p[i+1]==0 && p[i+2]==0 && p[i+3]==1) return 4;
    if (i + 3 <= n && p[i]==0 && p[i+1]==0 && p[i+2]==1) return 3;
    return 0;
}

// NAL boundary within the stream.
struct Nal { size_t off; size_t size; uint8_t type; };

// Scan the whole buffer for NAL units (start-code delimited).
std::vector<Nal> scan_nals(const uint8_t* p, size_t n) {
    std::vector<Nal> out;
    size_t i = 0;
    while (i < n) {
        size_t sc = start_code_len(p, n, i);
        if (!sc) { ++i; continue; }
        size_t start = i;
        size_t j = i + sc + 1;
        // Find next start code.
        while (j < n) {
            if (j + 3 <= n && p[j]==0 && p[j+1]==0 && (p[j+2]==0 || p[j+2]==1)) break;
            ++j;
        }
        Nal nal;
        nal.off  = start;
        nal.size = j - start;
        nal.type = (uint8_t)(p[start + sc] & 0x1f);
        out.push_back(nal);
        i = j;
    }
    return out;
}

// Group NALs into access units. With no B-frames + repeat_headers=1, a new AU
// starts when:
//   - An AUD (type 9) appears, OR
//   - A SPS (type 7) appears after a VCL NAL, OR
//   - A VCL NAL (1-5) appears after another VCL NAL
struct Au { size_t off; size_t size; };

std::vector<Au> group_access_units(const std::vector<Nal>& nals, size_t total) {
    std::vector<Au> aus;
    bool in_au = false;
    bool prev_vcl = false;
    size_t au_start = 0;

    for (auto& nal : nals) {
        bool is_vcl = (nal.type >= 1 && nal.type <= 5);
        bool is_aud = (nal.type == 9);
        bool is_sps = (nal.type == 7);

        bool new_au = false;
        if (!in_au) {
            new_au = true;
        } else if (is_aud) {
            new_au = true;
        } else if (is_vcl && prev_vcl) {
            new_au = true;  // second VCL in a row => new picture
        } else if (is_sps && prev_vcl) {
            new_au = true;  // SPS after a slice => new picture
        }

        if (new_au && in_au) {
            aus.push_back({au_start, nal.off - au_start});
        }
        if (new_au) {
            au_start = nal.off;
            in_au = true;
            prev_vcl = false;
        }
        if (is_vcl) prev_vcl = true;
        // AUD, SPS, PPS, SEI etc. don't set prev_vcl.
    }
    if (in_au) {
        aus.push_back({au_start, total - au_start});
    }
    return aus;
}

// Find the first SPS and PPS in the stream. Offsets/sizes EXCLUDE the start
// code (they start at the NAL header byte), so they can be passed directly to
// parse_sps and stored in the header for the player.
struct SpsPpsRef {
    uint32_t sps_off; uint16_t sps_size;
    uint32_t pps_off; uint16_t pps_size;
    bool found;
};

SpsPpsRef find_sps_pps(const uint8_t* stream, size_t total, const std::vector<Nal>& nals) {
    SpsPpsRef r{};
    for (auto& n : nals) {
        size_t sc = start_code_len(stream, total, n.off);
        if (n.type == 7 && !r.sps_size) {
            r.sps_off  = (uint32_t)(n.off + sc);   // past start code
            r.sps_size = (uint16_t)(n.size - sc);  // excludes start code
        } else if (n.type == 8 && !r.pps_size) {
            r.pps_off  = (uint32_t)(n.off + sc);
            r.pps_size = (uint16_t)(n.size - sc);
        }
    }
    r.found = r.sps_size && r.pps_size;
    return r;
}

} // namespace

ProcessResult process_h264_to_lwp(const std::string& h264_path,
                                  const std::string& lwp_path,
                                  const ProcessOptions& opt) {
    ProcessResult result;

    // --- Read the entire .h264 file ----------------------------------------
    FILE* f = std::fopen(h264_path.c_str(), "rb");
    if (!f) { result.error = "cannot open h264 file"; return result; }
    std::fseek(f, 0, SEEK_END);
    long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsz <= 0) { std::fclose(f); result.error = "empty h264 file"; return result; }
    std::vector<uint8_t> stream(fsz);
    if (std::fread(stream.data(), 1, fsz, f) != (size_t)fsz) {
        std::fclose(f); result.error = "read failed"; return result;
    }
    std::fclose(f);

    // --- Scan NALs ---------------------------------------------------------
    auto nals = scan_nals(stream.data(), stream.size());
    if (nals.empty()) { result.error = "no NALs found"; return result; }

    // --- Find SPS/PPS ------------------------------------------------------
    auto spspps = find_sps_pps(stream.data(), stream.size(), nals);
    if (!spspps.found) { result.error = "SPS/PPS not found"; return result; }

    // Parse SPS. sps_off already points past the start code, so the data
    // starts at the NAL header byte — exactly what parse_sps expects (it
    // skips the NAL header internally).
    SpsInfo si = parse_sps(stream.data() + spspps.sps_off, spspps.sps_size);
    if (!si.ok) { result.error = "SPS parse failed"; return result; }

    // --- Split into access units ------------------------------------------
    auto aus = group_access_units(nals, stream.size());
    if (aus.empty()) { result.error = "no AUs found"; return result; }

    // --- Determine fps -----------------------------------------------------
    // ffmpeg's -f h264 output doesn't carry container fps metadata. We rely
    // on the override_fps option (from the CLI -f flag). Default 30.
    uint16_t fps_num = opt.override_fps > 0 ? (uint16_t)opt.override_fps : 30;
    uint16_t fps_den = 1;

    // --- Write .lwp file ---------------------------------------------------
    // Layout: [LwpHeader] [AU index table] [payload]
    uint32_t au_table_count = (uint32_t)aus.size();
    uint32_t au_table_size  = au_table_count * (uint32_t)sizeof(AuEntry);

    LwpHeader hdr{};
    hdr.magic       = kMagic;
    hdr.header_size = kHeaderSize;
    hdr.width       = si.width;
    hdr.height      = si.height;
    hdr.fps_num     = fps_num;
    hdr.fps_den     = fps_den;
    hdr.frame_count = au_table_count;
    hdr.payload_size = (uint32_t)stream.size();
    hdr.profile_idc      = si.profile_idc;
    hdr.level_idc        = si.level_idc;
    hdr.bit_depth_luma   = si.bit_depth_luma;
    hdr.bit_depth_chroma = si.bit_depth_chroma;
    hdr.au_table_offset  = kHeaderSize;                  // right after header
    hdr.au_entry_size    = (uint16_t)sizeof(AuEntry);

    // SPS/PPS offsets relative to payload start (= 0, since payload = stream).
    hdr.sps_offset = (uint32_t)spspps.sps_off;
    hdr.sps_size   = (uint16_t)spspps.sps_size;
    hdr.pps_offset = (uint32_t)spspps.pps_off;
    hdr.pps_size   = (uint16_t)spspps.pps_size;

    FILE* out = std::fopen(lwp_path.c_str(), "wb");
    if (!out) { result.error = "cannot open output"; return result; }

    // 1. Header.
    std::fwrite(&hdr, sizeof(hdr), 1, out);

    // 2. AU index table. Offsets are relative to payload start.
    for (auto& au : aus) {
        AuEntry e;
        e.offset = (uint32_t)au.off;
        e.size   = (uint32_t)au.size;
        std::fwrite(&e, sizeof(e), 1, out);
    }

    // 3. Payload (the raw Annex B stream).
    std::fwrite(stream.data(), 1, stream.size(), out);
    std::fclose(out);

    result.ok = true;
    result.frame_count = au_table_count;
    result.width  = si.width;
    result.height = si.height;
    result.fps_num = fps_num;
    result.fps_den = fps_den;
    return result;
}

} // namespace lwp
