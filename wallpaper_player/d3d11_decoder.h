#pragma once
// D3D11 VideoDecoder wrapper for H.264 (DXVA).
//
// Inputs: raw H.264 Annex B access units (one picture per call to decode()).
// Outputs: an ID3D11Texture2D in NV12 format (shared, keyed-mutex) plus the
// subresource index of the decoded picture inside the decoder texture array.
//
// Assumptions (guaranteed by the maker):
//   - No B-frames (decode order == display order)
//   - Single SPS+PPS, fixed resolution
//   - One slice per picture
// These let us keep DXVA PicParams nearly constant and avoid reference
// picture reordering logic.

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "nal_parser.h"

namespace lwp {

using Microsoft::WRL::ComPtr;

struct DecoderConfig {
    SpsParams sps;
    // PPS raw bytes (Annex B payload, no start code, includes NAL header).
    std::vector<uint8_t> pps;
    // Parsed PPS fields (filled by the caller from `pps` above).
    PpsParams pps_parsed;
};

class D3D11VideoDecoder {
public:
    D3D11VideoDecoder() = default;
    ~D3D11VideoDecoder();

    D3D11VideoDecoder(const D3D11VideoDecoder&) = delete;
    D3D11VideoDecoder& operator=(const D3D11VideoDecoder&) = delete;

    // `device` is the D3D11 device used by the renderer (so output textures
    // are shareable). `cfg` carries the SPS+PPS parsed from the .lwp header.
    bool initialize(ID3D11Device* device, const DecoderConfig& cfg);

    // Release all decoder resources. Safe to call multiple times.
    void shutdown();

    // Decode one access unit. `au` is the concatenated NAL bytes WITH start
    // codes (00 00 00 01 ...). On success, `out_texture` is set to a *shared*
    // NV12 staging texture (CPU/GPU readable through keyed mutex) and
    // `out_subresource` is the index within the decoder's internal array
    // (currently unused externally since we copy out into a dedicated shared
    // texture each frame).
    bool decode(const uint8_t* au, size_t au_size,
                ComPtr<ID3D11Texture2D>* out_texture);

    uint16_t coded_width()  const { return coded_w_; }
    uint16_t coded_height() const { return coded_h_; }

private:
    bool configure_decoder_objects();
    bool submit_picture(const uint8_t* au, size_t au_size,
                        const SliceHeader& sh,
                        size_t slice_off, size_t slice_len);
    bool fill_pic_params(void* pic_params, const SliceHeader& sh,
                        uint8_t current_idx, uint8_t ref_idx);
    bool fill_bitstream(void* bs_data, uint32_t bs_size,
                        const uint8_t* au, size_t au_size,
                        uint32_t* out_bytes_written);

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    ctx_;
    ComPtr<ID3D11VideoDevice>      video_device_;
    ComPtr<ID3D11VideoContext>     video_ctx_;
    ComPtr<ID3D11VideoDecoder>     decoder_;
    // One output view per array slice — DecoderBeginFrame writes to the slice
    // specified by the view's ArraySlice, NOT by CurrPic.Index7Bits. CurrPic
    // is only used by the driver for reference-picture bookkeeping. Without
    // per-slice views, all frames decode into slice 0 and P-frame readback
    // from other slices returns all zeros (=> green screen after NV12->RGB).
    std::vector<ComPtr<ID3D11VideoDecoderOutputView>> decoder_output_views_;
    ComPtr<ID3D11Texture2D>        decode_textures_; // array of NV12 surfaces
    ComPtr<ID3D11Texture2D>        shared_output_;   // single shared NV12 copy

    D3D11_VIDEO_DECODER_CONFIG     decoder_cfg_{};
    GUID                           profile_guid_{};
    uint32_t                       surface_count_ = 0;

    DecoderConfig                  cfg_;
    uint16_t                       coded_w_ = 0;
    uint16_t                       coded_h_ = 0;
    uint8_t                        current_surface_ = 0;

    // Ref management (no B-frames => at most 1 ref frame).
    uint8_t                        ref_surface_ = 0xff; // 0xff = none
    uint16_t                       ref_frame_num_ = 0;   // previous frame's frame_num
    bool                           first_frame_ = true;
    uint32_t                       status_feedback_num_ = 0;
};

} // namespace lwp
