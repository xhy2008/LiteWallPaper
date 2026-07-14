#include "d3d11_decoder.h"

#include <dxgi.h>
#include <dxva.h>
#include <d3d11_4.h>
#include <algorithm>
#include <cstring>

namespace lwp {

namespace {

constexpr uint32_t kMaxSurfaces = 4; // no B-frames + 1 ref => 2-4 surfaces is enough

// H.264 VLD profiles (DXVA spec, all share the GUID stem
// A0C7-11D3-B984-00C04F2E73C5, differ only in Data1):
//   1B81BE64 = H264_VLD_NOFGT           (no film grain, any slice count)
//   1B81BE65 = H264_VLD_WITHFGT          (with film grain)
//   1B81BE66 = H264_VLD_NOMULTI_NOFGT    (no multi-slice, no FGT)
//   1B81BE67 = H264_VLD_MULTINOFGT       (multi-slice, no FGT)
//   1B81BE68 = H264_VLD_MULTIWITHFGT     (multi-slice, with FGT)
// All of them decode standard Annex-B H.264; the multi/FGT suffixes only
// indicate optional features the decoder *supports*, not features the
// bitstream must use. We accept any of them, preferring NOFGT variants.
#define H264_VLD_GUID(data1) { data1, 0xa0c7, 0x11d3, {0xb9,0x84,0x00,0xc0,0x4f,0x2e,0x73,0xc5} }
const GUID kProfileH264NoFgt        = H264_VLD_GUID(0x1b81be64);
const GUID kProfileH264WithFgt      = H264_VLD_GUID(0x1b81be65);
const GUID kProfileH264NoMultiNoFgt = H264_VLD_GUID(0x1b81be66);
const GUID kProfileH264MultiNoFgt   = H264_VLD_GUID(0x1b81be67);
const GUID kProfileH264MultiWithFgt = H264_VLD_GUID(0x1b81be68);
#undef H264_VLD_GUID

// Known H.264 VLD Data1 values.
bool is_h264_vld_data1(unsigned long d) {
    return d == 0x1b81be64 || d == 0x1b81be65 ||
           d == 0x1b81be66 || d == 0x1b81be67 || d == 0x1b81be68;
}

bool is_h264_vld_profile(const GUID& g) {
    return g.Data2 == 0xa0c7 && g.Data3 == 0x11d3 &&
           g.Data4[0]==0xb9 && g.Data4[1]==0x84 && g.Data4[2]==0x00 &&
           g.Data4[3]==0xc0 && g.Data4[4]==0x4f && g.Data4[5]==0x2e &&
           g.Data4[6]==0x73 && g.Data4[7]==0xc5 &&
           is_h264_vld_data1(g.Data1);
}

bool find_supported_profile(ID3D11VideoDevice* vdev, GUID* out) {
    UINT n = vdev->GetVideoDecoderProfileCount();
    // Preference order: NOFGT > MULTINOFGT > NOMULTI_NOFGT > WITHFGT > MULTIWITHFGT
    const GUID* prefs[] = {
        &kProfileH264NoFgt, &kProfileH264MultiNoFgt,
        &kProfileH264NoMultiNoFgt, &kProfileH264WithFgt,
        &kProfileH264MultiWithFgt
    };
    for (auto* pref : prefs) {
        for (UINT i = 0; i < n; ++i) {
            GUID g; if (FAILED(vdev->GetVideoDecoderProfile(i, &g))) continue;
            if (g == *pref) { *out = g; return true; }
        }
    }
    // Last resort: accept any H.264 VLD profile we recognize.
    for (UINT i = 0; i < n; ++i) {
        GUID g; if (FAILED(vdev->GetVideoDecoderProfile(i, &g))) continue;
        if (is_h264_vld_profile(g)) { *out = g; return true; }
    }
    return false;
}

bool pick_decoder_config(ID3D11VideoDevice* vdev,
                         const D3D11_VIDEO_DECODER_DESC& desc,
                         D3D11_VIDEO_DECODER_CONFIG* out) {
    UINT count = 0;
    if (FAILED(vdev->GetVideoDecoderConfigCount(&desc, &count)) || count == 0)
        return false;
    int best = -1, best_score = -1;
    for (UINT i = 0; i < count; ++i) {
        D3D11_VIDEO_DECODER_CONFIG cfg{};
        if (FAILED(vdev->GetVideoDecoderConfig(&desc, i, &cfg))) continue;
        // Prefer ConfigBitstreamRaw == 2 (whole-AU-per-buffer, raw byte stream).
        int score = 0;
        if (cfg.ConfigBitstreamRaw == 2)      score += 100;
        else if (cfg.ConfigBitstreamRaw == 1) score += 50;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) return false;
    return SUCCEEDED(vdev->GetVideoDecoderConfig(&desc, best, out));
}

} // namespace

D3D11VideoDecoder::~D3D11VideoDecoder() = default;

void D3D11VideoDecoder::shutdown() {
    decoder_output_views_.clear();
    decode_textures_.Reset();
    shared_output_.Reset();
    decoder_.Reset();
    video_ctx_.Reset();
    video_device_.Reset();
    ctx_.Reset();
    device_.Reset();
    current_surface_ = 0;
    ref_surface_ = 0xff;
    first_frame_ = true;
    status_feedback_num_ = 0;
    surface_count_ = 0;
}

bool D3D11VideoDecoder::initialize(ID3D11Device* device, const DecoderConfig& cfg) {
    device_ = device;
    device_->GetImmediateContext(&ctx_);
    cfg_ = cfg;
    coded_w_ = cfg.sps.coded_width;
    coded_h_ = cfg.sps.coded_height;

    if (FAILED(device_.As(&video_device_))) return false;
    if (FAILED(ctx_.As(&video_ctx_)))       return false;

    if (!find_supported_profile(video_device_.Get(), &profile_guid_)) {
        return false;
    }

    return configure_decoder_objects();
}

bool D3D11VideoDecoder::configure_decoder_objects() {
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = profile_guid_;
    desc.OutputFormat = DXGI_FORMAT_NV12;
    desc.SampleWidth  = coded_w_;
    desc.SampleHeight = coded_h_;
    // The decoder needs max_num_ref_frames + 1 surfaces minimum. With no
    // B-frames and 1 ref, 2 surfaces is the floor; we keep a small pool.
    surface_count_ = std::max<uint32_t>(kMaxSurfaces,
                                        cfg_.sps.max_num_ref_frames + 2);

    if (!pick_decoder_config(video_device_.Get(), desc, &decoder_cfg_)) {
        return false;
    }

    HRESULT hr = video_device_->CreateVideoDecoder(&desc, &decoder_cfg_, &decoder_);
    if (FAILED(hr)) return false;

    // Output texture array (DXVA writes decoded pictures into slices of this
    // array; the slice index is specified in DXVA_PicParams_H264::CurrPic).
    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width  = coded_w_;
    tex_desc.Height = coded_h_;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = surface_count_;
    tex_desc.Format = DXGI_FORMAT_NV12;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_DECODER;
    tex_desc.MiscFlags = 0;
    hr = device_->CreateTexture2D(&tex_desc, nullptr, &decode_textures_);
    if (FAILED(hr)) return false;

    // Create one output view per array slice. DecoderBeginFrame writes the
    // decoded picture into the slice specified by the view's ArraySlice, so
    // we must pass the view matching the current surface (cur). Using a single
    // view with ArraySlice=0 causes every frame to overwrite slice 0 while
    // CurrPic.Index7Bits tells the driver a different surface — P frames end
    // up referencing the wrong surface and readback returns all zeros.
    decoder_output_views_.resize(surface_count_);
    for (UINT i = 0; i < surface_count_; ++i) {
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vov_desc{};
        vov_desc.DecodeProfile  = profile_guid_;
        vov_desc.ViewDimension  = D3D11_VDOV_DIMENSION_TEXTURE2D;
        vov_desc.Texture2D.ArraySlice = i;
        hr = video_device_->CreateVideoDecoderOutputView(
                decode_textures_.Get(), &vov_desc,
                decoder_output_views_[i].GetAddressOf());
        if (FAILED(hr)) return false;
    }

    // Single NV12 output texture the renderer reads from. We copy each
    // decoded picture here from the decoder array texture. Same device, so
    // no keyed-mutex / shared handle needed.
    D3D11_TEXTURE2D_DESC shared_desc = tex_desc;
    shared_desc.ArraySize = 1;
    shared_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    shared_desc.MiscFlags = 0;
    hr = device_->CreateTexture2D(&shared_desc, nullptr, &shared_output_);
    if (FAILED(hr)) return false;

    first_frame_ = true;
    ref_surface_ = 0xff;
    return true;
}

bool D3D11VideoDecoder::decode(const uint8_t* au, size_t au_size,
                               ComPtr<ID3D11Texture2D>* out_texture) {
    if (!decoder_) return false;

    auto nals = split_nals(au, au_size);
    if (nals.empty()) return false;

    const NalUnit* vcl = nullptr;
    for (auto& n : nals) {
        if (n.type == NAL_SLICE || n.type == NAL_IDR) { vcl = &n; break; }
    }
    if (!vcl) return false;
    SliceHeader sh = parse_slice_header(vcl->data, vcl->size, cfg_.sps);
    if (!sh.ok) return false;

    if (sh.nal_type == NAL_IDR) ref_surface_ = 0xff;

    // Compute the slice NAL's location (including start code) within the AU.
    // vcl->data points after the start code; back up to find the start code.
    size_t nal_payload_off = (size_t)(vcl->data - au);
    size_t sc_len = 4;
    if (nal_payload_off >= 4 &&
        au[nal_payload_off-4] == 0x00 && au[nal_payload_off-3] == 0x00 &&
        au[nal_payload_off-2] == 0x00 && au[nal_payload_off-1] == 0x01) {
        sc_len = 4;
    } else if (nal_payload_off >= 3 &&
               au[nal_payload_off-3] == 0x00 && au[nal_payload_off-2] == 0x00 &&
               au[nal_payload_off-1] == 0x01) {
        sc_len = 3;
    }
    size_t slice_off = nal_payload_off - sc_len;
    size_t slice_len = au_size - slice_off;

    uint8_t cur = current_surface_;
    if (!submit_picture(au, au_size, sh, slice_off, slice_len)) {
        return false;
    }

    ref_surface_ = cur;
    ref_frame_num_ = sh.frame_num;
    current_surface_ = (current_surface_ + 1) % surface_count_;
    first_frame_ = false;

    // Copy decoded subresource (slice `cur` of the array texture) into the
    // single shared NV12 output texture the renderer reads from.
    ctx_->CopySubresourceRegion(shared_output_.Get(), 0, 0, 0, 0,
                                decode_textures_.Get(), cur, nullptr);

    *out_texture = shared_output_;
    return true;
}

bool D3D11VideoDecoder::submit_picture(const uint8_t* au, size_t au_size,
                                       const SliceHeader& sh,
                                       size_t slice_off, size_t slice_len) {
    uint8_t cur = current_surface_;
    uint8_t ref = (sh.nal_type == NAL_IDR) ? 0xff : ref_surface_;
    if (ref == 0xff && !first_frame_) ref = 0;

    HRESULT hr = video_ctx_->DecoderBeginFrame(decoder_.Get(),
                                               decoder_output_views_[cur].Get(),
                                               0, nullptr);
    if (FAILED(hr)) return false;

    // Picture parameters.
    void* pp_data = nullptr;
    UINT  pp_size = 0;
    if (FAILED(video_ctx_->GetDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &pp_size, &pp_data)) || !pp_data) {
        video_ctx_->DecoderEndFrame(decoder_.Get());
        return false;
    }
    bool ok = fill_pic_params(pp_data, sh, cur, ref);
    video_ctx_->ReleaseDecoderBuffer(decoder_.Get(),
        D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
    if (!ok) { video_ctx_->DecoderEndFrame(decoder_.Get()); return false; }

    // Inverse quantization matrix — send flat scaling (all 16). Some drivers
    // require this buffer for High profile, and x264 default uses no custom
    // scaling matrix, so flat (all-16) is the correct default.
    void* iq_data = nullptr;
    UINT  iq_size = 0;
    bool has_iq = false;
    if (SUCCEEDED(video_ctx_->GetDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
            &iq_size, &iq_data)) && iq_data) {
        // DXVA_Inverse_Quantization_Matrix_H264 is all UCHAR, flat = all 16.
        std::memset(iq_data, 16, iq_size);
        has_iq = true;
        video_ctx_->ReleaseDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);
    }

    // Bitstream (whole AU with start codes — ConfigBitstreamRaw==2).
    void* bs_data = nullptr;
    UINT  bs_size = 0;
    if (FAILED(video_ctx_->GetDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bs_size, &bs_data)) || !bs_data) {
        video_ctx_->DecoderEndFrame(decoder_.Get());
        return false;
    }
    uint32_t written = 0;
    ok = fill_bitstream(bs_data, bs_size, au, au_size, &written);
    video_ctx_->ReleaseDecoderBuffer(decoder_.Get(),
        D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
    if (!ok) { video_ctx_->DecoderEndFrame(decoder_.Get()); return false; }

    // Slice control. Use DXVA_Slice_H264_Short (the driver parses the slice
    // header itself from the raw bitstream). Long form is also valid for
    // ConfigBitstreamRaw==2 but Short is simpler and widely supported.
    void* sc_data = nullptr;
    UINT  sc_size = 0;
    UINT  sc_written = 0;
    bool has_sc = SUCCEEDED(video_ctx_->GetDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &sc_size, &sc_data)) && sc_data;
    if (has_sc) {
        auto* sc = static_cast<DXVA_Slice_H264_Short*>(sc_data);
        std::memset(sc, 0, sizeof(*sc));
        sc->BSNALunitDataLocation = (UINT)slice_off;
        sc->SliceBytesInBuffer     = (UINT)slice_len;
        sc->wBadSliceChopping      = 0;
        sc_written = sizeof(DXVA_Slice_H264_Short);
        video_ctx_->ReleaseDecoderBuffer(decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
    }

    // Build the buffer desc list. Order: PP, IQ (if present), BS, SC.
    // DataSize must be the ACTUAL data size, not the buffer capacity.
    D3D11_VIDEO_DECODER_BUFFER_DESC desc[4] = {};
    int ndesc = 0;
    desc[ndesc].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
    desc[ndesc].DataOffset = 0;
    desc[ndesc].DataSize   = sizeof(DXVA_PicParams_H264);
    ndesc++;
    if (has_iq) {
        desc[ndesc].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        desc[ndesc].DataOffset = 0;
        desc[ndesc].DataSize   = iq_size;
        ndesc++;
    }
    desc[ndesc].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    desc[ndesc].DataOffset = 0;
    desc[ndesc].DataSize   = written;
    ndesc++;
    if (has_sc) {
        desc[ndesc].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        desc[ndesc].DataOffset = 0;
        desc[ndesc].DataSize   = sc_written;
        ndesc++;
    }

    hr = video_ctx_->SubmitDecoderBuffers(decoder_.Get(), ndesc, desc);
    video_ctx_->DecoderEndFrame(decoder_.Get());
    return SUCCEEDED(hr);
}

bool D3D11VideoDecoder::fill_pic_params(void* pic_params, const SliceHeader& sh,
                                        uint8_t cur, uint8_t ref) {
    auto* p = static_cast<DXVA_PicParams_H264*>(pic_params);
    std::memset(p, 0, sizeof(*p));

    p->wFrameWidthInMbsMinus1  = cfg_.sps.pic_width_in_mbs  - 1;
    p->wFrameHeightInMbsMinus1 = (cfg_.sps.pic_height_in_mbs /
                                  (cfg_.sps.frame_mbs_only_flag ? 1 : 2)) - 1;

    // Use the named bitfield members directly (the union has an anonymous
    // struct with named fields). This is safer than manually building
    // wBitFields, whose bit layout varies across DXVA spec documents.
    p->field_pic_flag             = sh.field_pic_flag ? 1 : 0;
    p->MbaffFrameFlag             = 0; // frame_mbs_only => no MBAFF
    p->residual_colour_transform_flag = 0;
    p->sp_for_switch_flag         = 0;
    p->chroma_format_idc          = cfg_.sps.chroma_format_idc; // 1 = 4:2:0
    p->RefPicFlag                 = (sh.nal_ref_idc != 0) ? 1 : 0;
    p->constrained_intra_pred_flag = cfg_.pps_parsed.constrained_intra_pred_flag;
    p->weighted_pred_flag         = cfg_.pps_parsed.weighted_pred_flag;
    p->weighted_bipred_idc        = cfg_.pps_parsed.weighted_bipred_idc;
    p->MbsConsecutiveFlag         = 1; // single slice, all MBs consecutive
    p->frame_mbs_only_flag        = cfg_.sps.frame_mbs_only_flag ? 1 : 0;
    p->transform_8x8_mode_flag    = cfg_.pps_parsed.transform_8x8_mode_flag;
    p->MinLumaBipredSize8x8Flag  = 0;
    p->IntraPicFlag               = (sh.nal_type == NAL_IDR) ? 1 : 0;

    p->num_ref_frames = cfg_.sps.max_num_ref_frames;
    p->CurrPic.Index7Bits     = cur;
    p->CurrPic.AssociatedFlag = 0;

    // Compute picture order count. For poc_type=2 (x264 default), POC is
    // derived from frame_num: ref frame POC = 2*frame_num, non-ref = 2*frame_num+1.
    // For poc_type=0, POC comes from pic_order_cnt_lsb in the slice header.
    int32_t cur_poc = 0;
    if (cfg_.sps.pic_order_cnt_type == 0) {
        cur_poc = (int32_t)sh.pic_order_cnt_lsb;
    } else if (cfg_.sps.pic_order_cnt_type == 2) {
        cur_poc = 2 * (int32_t)sh.frame_num;
    }

    if (ref != 0xff) {
        p->RefFrameList[0].Index7Bits     = ref;
        p->RefFrameList[0].AssociatedFlag = 0;
        for (int i = 1; i < 16; ++i) {
            p->RefFrameList[i].Index7Bits     = 0x7f;
            p->RefFrameList[i].AssociatedFlag = 0;
        }
        // Reference POC: previous frame's POC.
        int32_t ref_poc = 0;
        if (cfg_.sps.pic_order_cnt_type == 0) {
            // With no B-frames, prev frame's poc_lsb was 2 less (assuming
            // sequential POC). But poc wraps, so compute relative.
            uint32_t poc_bits = 4 + cfg_.sps.log2_max_pic_order_cnt_lsb_minus4;
            uint32_t poc_mask = (1u << poc_bits) - 1;
            ref_poc = (int32_t)((sh.pic_order_cnt_lsb + poc_mask) & poc_mask);
            // Adjust for wrap: if ref_poc > cur_poc, it wrapped.
            if (ref_poc > cur_poc) ref_poc -= (int32_t)(poc_mask + 1);
        } else if (cfg_.sps.pic_order_cnt_type == 2) {
            ref_poc = 2 * ((int32_t)sh.frame_num - 1);
        }
        p->FieldOrderCntList[0][0] = ref_poc;
        p->FieldOrderCntList[0][1] = ref_poc;
    } else {
        for (int i = 0; i < 16; ++i) {
            p->RefFrameList[i].Index7Bits     = 0x7f;
            p->RefFrameList[i].AssociatedFlag = 0;
        }
    }
    p->CurrFieldOrderCnt[0] = cur_poc;
    p->CurrFieldOrderCnt[1] = cur_poc;

    p->bit_depth_luma_minus8              = cfg_.sps.bit_depth_luma - 8;
    p->bit_depth_chroma_minus8            = cfg_.sps.bit_depth_chroma - 8;
    p->log2_max_frame_num_minus4          = cfg_.sps.log2_max_frame_num_minus4;
    p->pic_order_cnt_type                 = cfg_.sps.pic_order_cnt_type;
    p->log2_max_pic_order_cnt_lsb_minus4  = cfg_.sps.log2_max_pic_order_cnt_lsb_minus4;
    p->delta_pic_order_always_zero_flag   = 0;
    p->direct_8x8_inference_flag          = 1;
    p->entropy_coding_mode_flag           = cfg_.pps_parsed.entropy_coding_mode_flag;
    p->pic_init_qp_minus26                = cfg_.pps_parsed.pic_init_qp_minus26;
    p->chroma_qp_index_offset             = cfg_.pps_parsed.chroma_qp_index_offset;
    p->second_chroma_qp_index_offset      = cfg_.pps_parsed.second_chroma_qp_index_offset;
    p->ContinuationFlag                   = 1;
    p->frame_num                          = sh.frame_num;
    p->num_ref_idx_l0_active_minus1       = 0; // 1 ref frame
    p->num_ref_idx_l1_active_minus1       = 0;
    p->deblocking_filter_control_present_flag = cfg_.pps_parsed.deblocking_filter_control_present_flag;
    p->redundant_pic_cnt_present_flag     = cfg_.pps_parsed.redundant_pic_cnt_present_flag;
    p->pic_order_present_flag             = cfg_.pps_parsed.bottom_field_pic_order_in_frame_present_flag;
    p->num_slice_groups_minus1            = cfg_.pps_parsed.num_slice_groups_minus1;
    p->Reserved16Bits                     = 0;

    // Status report feedback — must be non-zero and increment per frame.
    p->StatusReportFeedbackNumber = ++status_feedback_num_;

    // FrameNumList + UsedForReferenceFlags: tell the driver the frame_num of
    // each reference surface. With no B-frames and 1 ref, only entry 0 matters.
    if (ref != 0xff) {
        p->FrameNumList[0] = ref_frame_num_;
        p->UsedForReferenceFlags = 0x3; // entry 0 used as top+bottom ref
    } else {
        p->UsedForReferenceFlags = 0;
    }
    return true;
}

bool D3D11VideoDecoder::fill_bitstream(void* bs_data, uint32_t bs_size,
                                       const uint8_t* au, size_t au_size,
                                       uint32_t* out_written) {
    if (au_size > bs_size) return false;
    // For ConfigBitstreamRaw == 2 the driver expects a raw byte stream with
    // Annex B start codes intact (the whole AU including SPS/PPS if present).
    std::memcpy(bs_data, au, au_size);
    *out_written = (uint32_t)au_size;
    return true;
}

} // namespace lwp
