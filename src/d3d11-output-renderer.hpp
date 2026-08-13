#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <memory>
#include <cstdint>

constexpr size_t SHARED_BUFFER_COUNT = 3;

class D3D11OutputRenderer {
public:
    D3D11OutputRenderer();
    ~D3D11OutputRenderer();

    // Initialize a separate D3D11 device on the given adapter for scanout
    bool InitializeOnAdapter(IDXGIAdapter *adapter, uint32_t width, uint32_t height);

    // Bind scanout target texture from DisplaySurface shared handle
    bool BindScanoutTarget(HANDLE scanoutSharedHandle);

    // Create Triple-Buffer GPU-to-GPU cross-adapter shared texture pipeline (Zero-Copy & Zero-Lock)
    bool InitializeGPUSharedTexture();

    // Producer (GS thread): Copy OBS texture -> Shared texture on GPU (Non-blocking)
    bool ProducerCopyFrameGPU(gs_texture_t *obs_tex);

    // Consumer (Display thread): Copy Shared texture -> Scanout target on dedicated GPU device
    bool ConsumerPresentFrameGPU();

    // Fallback: Staging CPU Copy to scanout
    bool CopyFrameToScanout(gs_texture_t *obs_tex);

    void Cleanup();

    // Destroy stagesurf (must be called in OBS GS context)
    void DestroyStagesurf();

    bool IsDeviceRemoved() const;

private:
    // Separate D3D11 device for scanout (matches WinRT adapter)
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> scanout_texture_;

    // Triple-Buffered shared cross-adapter textures
    winrt::com_ptr<ID3D11Texture2D> obs_shared_textures_[SHARED_BUFFER_COUNT];
    winrt::com_ptr<ID3D11Texture2D> opened_shared_textures_[SHARED_BUFFER_COUNT];
    HANDLE shared_handles_[SHARED_BUFFER_COUNT]{ NULL, NULL, NULL };

    std::atomic<uint32_t> latest_produced_idx_{ 0 };
    uint32_t producer_idx_{ 0 };

    // Cached OBS stagesurf for GPU->CPU readback fallback
    gs_stagesurf_t *stagesurf_{ nullptr };

    uint32_t width_{ 0 };
    uint32_t height_{ 0 };
};
