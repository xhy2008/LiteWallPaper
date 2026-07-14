#pragma once
// Minimal H.264 bitstream helpers for the player.
//
// The maker guarantees: NO B-frames, one slice per picture, single SPS+PPS,
// Annex B byte stream with 00 00 00 01 start codes, repeat_headers=1.
//
// Under those guarantees, the player needs only:
//   1. A NAL splitter that groups NAL units into access units (one AU == one
//      picture, terminated by the next VCL NAL or AUD).
//   2. Parse the SPS once for: profile, level, pic_width/height_in_mbs,
//      max_num_ref_frames, log2_max_frame_num, log2_max_poc_lsb,
//      chroma_format_idc, pic_order_cnt_type.
//   3. Parse each slice header for: first_mb_in_slice, frame_num,
//      pic_order_cnt_lsb (or delta), slice_type (I/P). With no B-frames and
//      one ref frame, the DXVA PicParams can be filled trivially.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace lwp {

enum NalUnitType : uint8_t {
    NAL_SLICE    = 1,
    NAL_IDR      = 5,
    NAL_SPS      = 7,
    NAL_PPS      = 8,
    NAL_AUD      = 9,
};

struct NalUnit {
    const uint8_t* data;   // points into the source buffer (NO start code)
    size_t size;           // bytes (NO start code)
    uint8_t type;          // nal_unit_type (low 5 bits of NAL header)
    uint8_t nal_ref_idc;   // high 2 bits of NAL header
};

// Split an Annex B byte stream into NAL units. Start codes may be 3 or 4 bytes.
// The returned NalUnit pointers reference `stream`, so `stream` must outlive
// them. `stream_size` bytes are scanned.
std::vector<NalUnit> split_nals(const uint8_t* stream, size_t stream_size);

// SPS fields the decoder cares about.
struct SpsParams {
    bool     ok = false;
    uint16_t profile_idc      = 0;
    uint8_t  level_idc        = 0;
    uint8_t  chroma_format_idc= 1;   // 1 = 4:2:0
    uint8_t  bit_depth_luma   = 8;
    uint8_t  bit_depth_chroma = 8;
    uint16_t pic_width_in_mbs = 0;   // number of 16x16 macroblocks horizontally
    uint16_t pic_height_in_mbs= 0;
    uint16_t coded_width      = 0;   // pic_width_in_mbs * 16
    uint16_t coded_height     = 0;   // pic_height_in_mbs * 16
    uint32_t max_num_ref_frames = 1;
    uint32_t log2_max_frame_num_minus4 = 0;
    uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
    uint32_t pic_order_cnt_type = 0;
    bool     frame_mbs_only_flag = true;
};

SpsParams parse_sps(const uint8_t* nal_payload, size_t size);

// Slice header fields needed for DXVA PicParams. With no B-frames, one ref,
// and one slice per picture, only a handful matter.
struct SliceHeader {
    bool     ok = false;
    uint8_t  nal_type;          // 1=non-IDR P slice, 5=IDR
    uint8_t  nal_ref_idc;
    uint32_t first_mb_in_slice  = 0;
    uint32_t slice_type         = 0;   // 0=P, 2=I, 5=P(all), 7=I(all)
    uint32_t frame_num          = 0;
    uint32_t idr_pic_id         = 0;
    uint32_t pic_order_cnt_lsb  = 0;   // valid if poc_type==0
    int32_t  delta_pic_order_cnt0 = 0; // valid if poc_type==1 & that flag set
    bool     field_pic_flag     = false;
    bool     bottom_field_flag  = false;
};

// `nal_payload` excludes the start code and the single NAL header byte is
// INCLUDED in `size` (i.e. nal_payload[0] == NAL header). `sps` is needed to
// interpret frame_num/poc widths.
SliceHeader parse_slice_header(const uint8_t* nal_payload, size_t size,
                               const SpsParams& sps);

// PPS fields needed for DXVA PicParams. Without these the decoder would have
// to guess entropy_coding_mode_flag, qp offsets, transform_8x8_mode_flag,
// etc. — and the driver rejects mismatched values with E_FAIL.
struct PpsParams {
    bool     ok = false;
    uint8_t  entropy_coding_mode_flag       = 0; // 0=CAVLC, 1=CABAC
    uint8_t  bottom_field_pic_order_in_frame_present_flag = 0;
    uint8_t  num_slice_groups_minus1        = 0;
    uint8_t  weighted_pred_flag             = 0;
    uint8_t  weighted_bipred_idc            = 0;
    int8_t   pic_init_qp_minus26            = 0;
    int8_t   chroma_qp_index_offset         = 0;
    uint8_t  deblocking_filter_control_present_flag = 1;
    uint8_t  constrained_intra_pred_flag    = 0;
    uint8_t  redundant_pic_cnt_present_flag = 0;
    uint8_t  transform_8x8_mode_flag        = 0;
    int8_t   second_chroma_qp_index_offset  = 0;
};

// `nal_payload` includes the 1-byte NAL header (nal_payload[0] == NAL header).
// `profile_idc` from the SPS is needed to decide whether to parse the High
// profile PPS extension (transform_8x8, scaling matrix, second chroma qp).
PpsParams parse_pps(const uint8_t* nal_payload, size_t size,
                    uint16_t profile_idc);

} // namespace lwp
