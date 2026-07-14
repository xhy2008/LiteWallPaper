#include "sps_parser.h"

#include <vector>

namespace lwp {

namespace {

// Exp-Golomb unsigned reader over an emulation-prevention-stripped buffer.
struct BitReader {
    const uint8_t* p;
    size_t size;
    size_t bytepos = 0;
    int bitpos = 7; // MSB first

    BitReader(const uint8_t* p_, size_t s) : p(p_), size(s) {}

    int bit() {
        if (bytepos >= size) return 0;
        int v = (p[bytepos] >> bitpos) & 1;
        if (--bitpos < 0) { bitpos = 7; ++bytepos; }
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (bytepos < size && bit() == 0) {
            if (++zeros > 31) return 0; // safety
        }
        uint32_t v = 1;
        for (int i = 0; i < zeros; ++i) v = (v << 1) | bit();
        return v - 1;
    }
    int32_t se() {
        uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }
    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
};

std::vector<uint8_t> strip_emulation_prevention(const uint8_t* src, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (i + 2 < size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0x03) {
            out.push_back(0);
            out.push_back(0);
            i += 2; // skip 0x03
        } else {
            out.push_back(src[i]);
        }
    }
    return out;
}

} // namespace

SpsInfo parse_sps(const uint8_t* nal, size_t size) {
    SpsInfo info;
    // Skip the 1-byte NAL header (forbidden_zero_bit | nal_ref_idc | type).
    if (size < 4) return info;
    auto rbsp = strip_emulation_prevention(nal + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());

    info.profile_idc = (uint16_t)br.u(8);
    /*constraint_set0..5 + reserved_zero2bits*/ br.u(8);
    info.level_idc = (uint8_t)br.u(8);
    br.ue(); // seq_parameter_set_id

    if (info.profile_idc == 100 || info.profile_idc == 110 ||
        info.profile_idc == 122 || info.profile_idc == 244 ||
        info.profile_idc ==  44 || info.profile_idc ==  83 ||
        info.profile_idc ==  86 || info.profile_idc == 118 ||
        info.profile_idc == 128 || info.profile_idc == 138 ||
        info.profile_idc == 139 || info.profile_idc == 134 ||
        info.profile_idc == 135) {
        uint32_t chroma_format_idc = br.ue();
        if (chroma_format_idc == 3) br.u(1); // separate_colour_plane_flag
        br.ue(); // bit_depth_luma_minus8
        br.ue(); // bit_depth_chroma_minus8
        br.u(1); // qpprime_y_zero_transform_bypass_flag
        uint32_t seq_scaling_matrix_present = br.u(1);
        if (seq_scaling_matrix_present) {
            int n = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < n; ++i) {
                if (br.u(1)) { br.ue(); br.ue(); } // skip list
            }
        }
    }
    br.ue(); // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = br.ue();
    if (pic_order_cnt_type == 0) {
        br.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        br.u(1); br.se(); br.se();
        uint32_t n = br.ue();
        for (uint32_t i = 0; i < n; ++i) br.se();
    }
    br.ue(); // max_num_ref_frames
    br.u(1); // gaps_in_frame_num_value_allowed_flag
    uint32_t pic_width_in_mbs_minus1  = br.ue();
    uint32_t pic_height_in_map_units_minus1 = br.ue();
    uint32_t frame_mbs_only_flag = br.u(1);
    if (!frame_mbs_only_flag) br.u(1); // mb_adaptive_frame_field_flag
    br.u(1); // direct_8x8_inference_flag

    info.width  = (uint16_t)((pic_width_in_mbs_minus1 + 1) * 16);
    info.height = (uint16_t)((pic_height_in_map_units_minus1 + 1) * 16 *
                             (frame_mbs_only_flag ? 1 : 2));
    info.ok = true;
    return info;
}

} // namespace lwp
