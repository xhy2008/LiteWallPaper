#pragma once
// Minimal H.264 SPS parser. Only the fields the .lwp header needs are read:
// profile_idc, level_idc, bit_depth_luma_minus8, bit_depth_chroma_minus8,
// and the coded width/height (pic_width_in_mbs / pic_height_in_mbs_units).
//
// This is NOT a full SPS parser — it assumes Main/High/Baseline profile with
// no exotic features (separate_colour_plane_flag etc.). That is fine because
// the maker produces the SPS itself with a known, simple encoder config.

#include <cstdint>
#include <cstddef>

namespace lwp {

struct SpsInfo {
    uint16_t profile_idc   = 0;
    uint8_t  level_idc     = 0;
    uint8_t  bit_depth_luma   = 8;
    uint8_t  bit_depth_chroma = 8;
    uint16_t width          = 0;   // coded width  (rounded up to MB)
    uint16_t height         = 0;   // coded height (rounded up to MB)
    bool     ok             = false;
};

// `nal` points at the SPS NAL unit data (AFTER the start code, INCLUDING the
// 1-byte NAL header). `size` is the NAL size excluding the start code.
// The NAL header byte is skipped internally; emulation prevention bytes
// (0x03) are also stripped internally.
SpsInfo parse_sps(const uint8_t* nal, size_t size);

} // namespace lwp
