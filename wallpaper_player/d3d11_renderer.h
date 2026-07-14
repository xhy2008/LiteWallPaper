#pragma once
// Renders an NV12 D3D11 texture (the decoder output) into the swap chain
// associated with the wallpaper window. Uses a single fullscreen triangle
// with an NV12->RGB pixel shader. Keeps geometry and shaders resident to
// avoid per-frame allocation.

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace lwp {

using Microsoft::WRL::ComPtr;

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    // `hwnd` is the wallpaper window (child of WorkerW). The renderer creates
    // a DXGI swap chain on it. `video_width/height` is the coded picture size
    // (used only to validate that incoming NV12 textures match).
    bool initialize(HWND hwnd, uint16_t video_width, uint16_t video_height);
    void shutdown();

    // Resize swap chain to match the window's client area.
    bool resize(uint32_t width, uint32_t height);

    // Render one frame from `nv12` (must have been produced by the decoder).
    // `fill` = stretch to fill (ignore aspect). `fill` = false = letterbox.
    void render(ID3D11Texture2D* nv12, bool fill = true);

    // Present with vsync on (SyncInterval=1) for steady pacing; cheap because
    // the OS desktop compositor is the only thing on screen.
    void present();

    ID3D11Device* device() const { return device_.Get(); }
    bool initialized() const { return device_ != nullptr; }
    uint16_t video_width() const { return video_w_; }
    uint16_t video_height() const { return video_h_; }

private:
    bool create_shaders();
    bool create_pipeline_state(uint16_t vw, uint16_t vh);

    ComPtr<ID3D11Device>          device_;
    ComPtr<ID3D11DeviceContext>   ctx_;
    ComPtr<IDXGISwapChain>        swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader>    vs_;
    ComPtr<ID3D11PixelShader>     ps_;
    ComPtr<ID3D11SamplerState>    sampler_;
    ComPtr<ID3D11BlendState>      blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    // Cached SRVs for the current NV12 texture. Recreated only when the
    // decoder hands us a different texture (e.g. after a wallpaper switch).
    ComPtr<ID3D11ShaderResourceView> y_srv_;
    ComPtr<ID3D11ShaderResourceView> uv_srv_;
    ID3D11Texture2D* cached_nv12_ = nullptr; // raw ptr for identity check only

    HWND    hwnd_         = nullptr;
    uint32_t target_w_     = 0;
    uint32_t target_h_     = 0;
    uint16_t video_w_      = 0;
    uint16_t video_h_      = 0;
};

} // namespace lwp
