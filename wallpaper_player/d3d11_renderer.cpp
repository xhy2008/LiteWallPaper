#include "d3d11_renderer.h"

#include <dxgi.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace lwp {

namespace {



// Fullscreen-triangle vertex shader. Generates a triangle covering clip space
// [-1,1]x[-1,1] from a vertex index in SV_VertexID, no vertex buffer needed.
const char kVsSrc[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    // 0 -> (-1, -1), 1 -> (-1, 3), 2 -> (3, -1)
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pixel shader: sample NV12 planes (Y plane + interleaved UV plane) and
// convert to RGB using BT.601. The shader expects the texture bound as a
// Texture2D array (NV12 in D3D11 exposes two SRVs: plane 0 = Y, plane 1 = UV).
const char kPsSrc[] = R"(
Texture2D<float4> yPlane  : register(t0);
Texture2D<float4> uvPlane : register(t1);
SamplerState      smp     : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y  = yPlane.Sample(smp, uv).r;
    float2 uv2 = uvPlane.Sample(smp, uv).rg;
    float u = uv2.x - 0.5f;
    float v = uv2.y - 0.5f;

    // BT.601 limited-range (x264 output is limited-range by default).
    y = (y - (16.0f/255.0f)) * (255.0f/219.0f);
    u = u * (255.0f/224.0f);
    v = v * (255.0f/224.0f);

    float r = y            + 1.402f * v;
    float g = y - 0.344f * u - 0.714f * v;
    float b = y + 1.772f * u;
    return float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
)";

bool compile_shader(const char* src, const char* entry, const char* target,
                    ID3DBlob** out_blob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out_blob, &err);
    if (FAILED(hr) && err) {
        OutputDebugStringA((const char*)err->GetBufferPointer());
    }
    return SUCCEEDED(hr);
}

} // namespace

D3D11Renderer::~D3D11Renderer() { shutdown(); }

bool D3D11Renderer::initialize(HWND hwnd, uint16_t vw, uint16_t vh) {
    hwnd_ = hwnd;
    video_w_ = vw;
    video_h_ = vh;

    RECT rc; GetClientRect(hwnd, &rc);
    target_w_ = (uint32_t)(rc.right  - rc.left);
    target_h_ = (uint32_t)(rc.bottom - rc.top);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, fls, 1, D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), nullptr,
                                 ctx_.GetAddressOf()))) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(device_.As(&dxgi_dev))) return false;
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = target_w_ ? target_w_ : 1;
    sd.BufferDesc.Height = target_h_ ? target_h_ : 1;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // No ALLOW_TEARING: the wallpaper window is a child of WorkerW after
    // SetParent, and child windows generally don't support tearing. We
    // present with SyncInterval=1 (vsync) anyway, so tearing is irrelevant.
    sd.Flags = 0;
    if (FAILED(factory->CreateSwapChain(device_.Get(), &sd, swapchain_.GetAddressOf()))) {
        return false;
    }

    if (!create_shaders()) return false;
    if (!create_pipeline_state(vw, vh)) return false;

    // Sampler (linear, clamp).
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&samp, sampler_.GetAddressOf());

    // No blending, no culling, wireframe off.
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = 0xf;
    device_->CreateBlendState(&bd, blend_.GetAddressOf());

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, raster_.GetAddressOf());

    return resize(target_w_, target_h_);
}

bool D3D11Renderer::create_shaders() {
    ComPtr<ID3DBlob> vs_blob, ps_blob;
    if (!compile_shader(kVsSrc, "main", "vs_5_0", vs_blob.GetAddressOf())) return false;
    if (!compile_shader(kPsSrc, "main", "ps_5_0", ps_blob.GetAddressOf())) return false;
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(),
            vs_blob->GetBufferSize(), nullptr, vs_.GetAddressOf()))) return false;
    if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(),
            ps_blob->GetBufferSize(), nullptr, ps_.GetAddressOf()))) return false;
    return true;
}

bool D3D11Renderer::create_pipeline_state(uint16_t vw, uint16_t vh) {
    (void)vw; (void)vh;
    // No vertex buffer / input layout needed (fullscreen triangle from SV_VertexID).
    return true;
}

bool D3D11Renderer::resize(uint32_t w, uint32_t h) {
    if (!swapchain_) return false;
    target_w_ = w; target_h_ = h;
    rtv_.Reset();
    HRESULT hr = swapchain_->ResizeBuffers(0, w ? w : 1, h ? h : 1,
                                           DXGI_FORMAT_B8G8R8A8_UNORM,
                                           0);
    if (FAILED(hr)) return false;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())))) return false;
    if (FAILED(device_->CreateRenderTargetView(back.Get(), nullptr,
                                               rtv_.GetAddressOf()))) return false;
    return true;
}

void D3D11Renderer::render(ID3D11Texture2D* nv12, bool fill) {
    if (!nv12 || !rtv_) return;

    // Recreate the SRVs only when the input texture changes (e.g. after a
    // wallpaper switch). The decoder reuses the same shared_output_ texture
    // across frames, so this check avoids creating 2 SRVs every frame.
    if (cached_nv12_ != nv12) {
        y_srv_.Reset();
        uv_srv_.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        srv_desc.Format = DXGI_FORMAT_R8_UNORM;
        HRESULT hr_y = device_->CreateShaderResourceView(nv12, &srv_desc, y_srv_.GetAddressOf());
        srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        HRESULT hr_uv = device_->CreateShaderResourceView(nv12, &srv_desc, uv_srv_.GetAddressOf());

        if (FAILED(hr_y) || FAILED(hr_uv)) return;
        cached_nv12_ = nv12;
    }

    ID3D11ShaderResourceView* srvs[2] = { y_srv_.Get(), uv_srv_.Get() };

    float clear[4] = { 0.f, 0.f, 0.f, 1.f };
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{};
    if (fill) {
        vp.Width = (float)target_w_;
        vp.Height = (float)target_h_;
        vp.MaxDepth = 1.f;
    } else {
        // Letterbox preserving video aspect.
        float ar_v = (float)video_w_ / video_h_;
        float ar_t = (float)target_w_ / target_h_;
        if (ar_v > ar_t) {
            vp.Width  = (float)target_w_;
            vp.Height = (float)target_w_ / ar_v;
            vp.TopLeftY = ((float)target_h_ - vp.Height) * 0.5f;
        } else {
            vp.Width  = (float)target_h_ * ar_v;
            vp.Height = (float)target_h_;
            vp.TopLeftX = ((float)target_w_ - vp.Width) * 0.5f;
        }
        vp.MaxDepth = 1.f;
    }
    ctx_->RSSetViewports(1, &vp);

    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx_->Draw(3, 0);

    // Unbind so the decoder can write to the texture next frame.
    ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
    ctx_->PSSetShaderResources(0, 2, null_srvs);
}

void D3D11Renderer::present() {
    if (swapchain_) {
        swapchain_->Present(1, 0);
    }
}

void D3D11Renderer::shutdown() {
    y_srv_.Reset();
    uv_srv_.Reset();
    cached_nv12_ = nullptr;
    raster_.Reset();
    blend_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    rtv_.Reset();
    swapchain_.Reset();
    ctx_.Reset();
    device_.Reset();
}

} // namespace lwp
