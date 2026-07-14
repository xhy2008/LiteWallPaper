#include "nal_parser.h"

#include <algorithm>

namespace lwp {

namespace {

struct BitReader {
    const uint8_t* p;
    size_t size;
    size_t bytepos = 0;
    int bitpos = 7;
    explicit BitReader(const uint8_t* p_, size_t s) : p(p_), size(s) {}

    int bit() {
        if (bytepos >= size) return 0;
        int v = (p[bytepos] >> bitpos) & 1;
        if (--bitpos < 0) { bitpos = 7; ++bytepos; }
        return v;
    }
    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (bytepos < size && bit() == 0) if (++zeros > 31) return 0;
        uint32_t v = 1;
        for (int i = 0; i < zeros; ++i) v = (v << 1) | bit();
        return v - 1;
    }
    int32_t se() {
        uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }
};

// In-place emulation-prevention stripping. Returns a buffer with 0x03 bytes
// removed from 00 00 03 sequences. Also returns, for each byte in the OUTPUT
// buffer, which byte it came from in the INPUT — not needed here, since we
// only parse the small slice header which sits at the very start of the NAL.
std::vector<uint8_t> strip_ep(const uint8_t* src, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i + 2 < n && src[i] == 0 && src[i+1] == 0 && src[i+2] == 0x03) {
            out.push_back(0); out.push_back(0);
            i += 2;
        } else {
            out.push_back(src[i]);
        }
    }
    return out;
}

} // namespace

std::vector<NalUnit> split_nals(const uint8_t* stream, size_t n) {
    std::vector<NalUnit> out;
    size_t i = 0;
    while (i < n) {
        size_t sc = 0;
        if (i + 4 <= n && stream[i]==0 && stream[i+1]==0 && stream[i+2]==0 && stream[i+3]==1) sc = 4;
        else if (i + 3 <= n && stream[i]==0 && stream[i+1]==0 && stream[i+2]==1) sc = 3;
        else { ++i; continue; }
        size_t nal_start = i + sc;
        size_t j = nal_start + 1;
        while (j < n) {
            // Look ahead for the next start code.
            if (j + 3 <= n && stream[j]==0 && stream[j+1]==0 &&
                (stream[j+2]==0 || stream[j+2]==1)) break;
            ++j;
        }
        NalUnit nu;
        nu.data = stream + nal_start;
        nu.size = j - nal_start;
        if (nu.size >= 1) {
            nu.nal_ref_idc = (nu.data[0] >> 5) & 0x3;
            nu.type        = nu.data[0] & 0x1f;
            out.push_back(nu);
        }
        i = j;
    }
    return out;
}

SpsParams parse_sps(const uint8_t* nal_payload, size_t size) {
    SpsParams s;
    // Skip NAL header byte.
    if (size < 4) return s;
    auto rbsp = strip_ep(nal_payload + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());

    s.profile_idc = (uint16_t)br.u(8);
    br.u(8); // constraint flags + reserved
    s.level_idc   = (uint8_t)br.u(8);
    br.ue(); // seq_parameter_set_id

    if (s.profile_idc == 100 || s.profile_idc == 110 ||
        s.profile_idc == 122 || s.profile_idc == 244 ||
        s.profile_idc ==  44 || s.profile_idc ==  83 ||
        s.profile_idc ==  86 || s.profile_idc == 118 ||
        s.profile_idc == 128 || s.profile_idc == 138 ||
        s.profile_idc == 139 || s.profile_idc == 134 ||
        s.profile_idc == 135) {
        s.chroma_format_idc = (uint8_t)br.ue();
        if (s.chroma_format_idc == 3) br.u(1); // separate_colour_plane
        s.bit_depth_luma   = (uint8_t)(br.ue() + 8);
        s.bit_depth_chroma = (uint8_t)(br.ue() + 8);
        br.u(1); // qpprime_y_zero_transform_bypass
        if (br.u(1)) { // seq_scaling_matrix_present
            int cnt = (s.chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < cnt; ++i) if (br.u(1)) { br.ue(); br.ue(); }
        }
    }
    s.log2_max_frame_num_minus4 = br.ue();
    s.pic_order_cnt_type = br.ue();
    if (s.pic_order_cnt_type == 0) {
        s.log2_max_pic_order_cnt_lsb_minus4 = br.ue();
    } else if (s.pic_order_cnt_type == 1) {
        br.u(1); br.se(); br.se();
        uint32_t n = br.ue();
        for (uint32_t i = 0; i < n; ++i) br.se();
    }
    s.max_num_ref_frames = br.ue();
    br.u(1); // gaps_in_frame_num_allowed
    uint32_t pwim1 = br.ue(); // pic_width_in_mbs_minus1
    uint32_t phim1 = br.ue(); // pic_height_in_map_units_minus1
    s.frame_mbs_only_flag = (bool)br.u(1);
    if (!s.frame_mbs_only_flag) br.u(1); // mb_adaptive_frame_field
    br.u(1); // direct_8x8_inference

    s.pic_width_in_mbs  = (uint16_t)(pwim1 + 1);
    s.pic_height_in_mbs = (uint16_t)(phim1 + 1) * (s.frame_mbs_only_flag ? 1 : 2);
    s.coded_width  = (uint16_t)(s.pic_width_in_mbs  * 16);
    s.coded_height = (uint16_t)(s.pic_height_in_mbs * 16);
    s.ok = true;
    return s;
}

SliceHeader parse_slice_header(const uint8_t* nal_payload, size_t size,
                               const SpsParams& sps) {
    SliceHeader h;
    if (size < 1) return h;
    h.nal_type    = nal_payload[0] & 0x1f;
    h.nal_ref_idc = (nal_payload[0] >> 5) & 0x3;

    auto rbsp = strip_ep(nal_payload + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());

    h.first_mb_in_slice = br.ue();
    h.slice_type        = br.ue();
    /*pps_id*/ br.ue();
    h.frame_num = br.u(4 + sps.log2_max_frame_num_minus4);

    if (!sps.frame_mbs_only_flag) {
        h.field_pic_flag = (bool)br.u(1);
        if (h.field_pic_flag) h.bottom_field_flag = (bool)br.u(1);
    }
    if (h.nal_type == NAL_IDR) {
        h.idr_pic_id = br.ue();
    }
    if (sps.pic_order_cnt_type == 0) {
        h.pic_order_cnt_lsb = br.u(4 + sps.log2_max_pic_order_cnt_lsb_minus4);
    } else if (sps.pic_order_cnt_type == 1 && false) {
        // Not used by our maker (x264 default uses poc_type=2 actually).
    }
    // We do not need to parse further: ref_pic_list_modification, dec_ref_pic_marking,
    // etc., because with max_num_ref_frames<=1 and no B-frames, the default
    // sliding-window reference management is sufficient and DXVA handles it.
    h.ok = true;
    return h;
}

PpsParams parse_pps(const uint8_t* nal_payload, size_t size,
                    uint16_t profile_idc) {
    PpsParams pps;
    if (size < 2) return pps;
    auto rbsp = strip_ep(nal_payload + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());

    /*pps_id*/  br.ue();
    /*sps_id*/  br.ue();
    pps.entropy_coding_mode_flag = (uint8_t)br.u(1);
    pps.bottom_field_pic_order_in_frame_present_flag = (uint8_t)br.u(1);
    pps.num_slice_groups_minus1 = (uint8_t)br.ue();
    // Maker guarantees single slice group, so we don't parse slice_group_map.
    if (pps.num_slice_groups_minus1 > 0) return pps; // unsupported, leave ok=false
    /*num_ref_idx_l0_default_active_minus1*/ br.ue();
    /*num_ref_idx_l1_default_active_minus1*/ br.ue();
    pps.weighted_pred_flag  = (uint8_t)br.u(1);
    pps.weighted_bipred_idc = (uint8_t)br.u(2);
    pps.pic_init_qp_minus26    = (int8_t)br.se();
    /*pic_init_qs_minus26*/ br.se();
    pps.chroma_qp_index_offset = (int8_t)br.se();
    pps.deblocking_filter_control_present_flag = (uint8_t)br.u(1);
    pps.constrained_intra_pred_flag    = (uint8_t)br.u(1);
    pps.redundant_pic_cnt_present_flag = (uint8_t)br.u(1);

    // The High profile PPS extension (transform_8x8, scaling matrix,
    // second_chroma_qp_index_offset) is only present for High profile and
    // above. For Baseline/Main profile, the remaining bits are just the RBSP
    // stop bit + alignment zeros — parsing them as extension data would
    // incorrectly set transform_8x8_mode_flag=1 (the stop bit), causing DXVA
    // to mismatch the bitstream and silently fail.
    bool is_high_profile = (profile_idc == 100 || profile_idc == 110 ||
                            profile_idc == 122 || profile_idc == 244 ||
                            profile_idc ==  44 || profile_idc ==  83 ||
                            profile_idc ==  86 || profile_idc == 118 ||
                            profile_idc == 128 || profile_idc == 138 ||
                            profile_idc == 139 || profile_idc == 134 ||
                            profile_idc == 135);
    if (is_high_profile) {
        // more_rbsp_data(): check if there's at least one data bit before the
        // trailing stop bit (last 1-bit). We scan the remaining bits; if there
        // are two or more 1-bits, at least one is data (the other is stop).
        int ones = 0;
        size_t bp = br.bytepos;
        int bpos = br.bitpos;
        while (bp < rbsp.size()) {
            if ((rbsp[bp] >> bpos) & 1) ++ones;
            if (--bpos < 0) { bpos = 7; ++bp; }
        }
        if (ones >= 2) {
            pps.transform_8x8_mode_flag = (uint8_t)br.u(1);
            if (br.u(1)) { // pic_scaling_matrix_present_flag
                // We don't need the actual matrices (we use flat IQ), just skip.
                int n = 12; // worst case for High 4:4:4; for 4:2:0 it's 8/12
                for (int i = 0; i < n; ++i) if (br.u(1)) { br.ue(); br.ue(); }
            }
            pps.second_chroma_qp_index_offset = (int8_t)br.se();
        }
    }
    pps.ok = true;
    return pps;
}

} // namespace lwp
