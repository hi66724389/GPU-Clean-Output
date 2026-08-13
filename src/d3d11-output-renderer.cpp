#include "d3d11-output-renderer.hpp"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <algorithm>
#include <cstring>

D3D11OutputRenderer::D3D11OutputRenderer() {}

D3D11OutputRenderer::~D3D11OutputRenderer() {
    Cleanup();
}

void D3D11OutputRenderer::Cleanup() {
    for (size_t i = 0; i < SHARED_BUFFER_COUNT; ++i) {
        opened_shared_textures_[i] = nullptr;
        obs_shared_textures_[i] = nullptr;
        if (shared_handles_[i]) {
            CloseHandle(shared_handles_[i]);
            shared_handles_[i] = NULL;
        }
    }

    scanout_texture_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
    blog(LOG_INFO, "[obs-clean-output] D3D11OutputRenderer cleaned up.");
}

void D3D11OutputRenderer::DestroyStagesurf() {
    if (stagesurf_) {
        gs_stagesurface_destroy(stagesurf_);
        stagesurf_ = nullptr;
    }
}

bool D3D11OutputRenderer::IsDeviceRemoved() const {
    if (!device_) return true;
    HRESULT hr = device_->GetDeviceRemovedReason();
    return FAILED(hr);
}

bool D3D11OutputRenderer::InitializeOnAdapter(IDXGIAdapter *adapter, uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL out_level;

    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN, // Must use UNKNOWN when specifying adapter
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        device_.put(),
        &out_level,
        context_.put()
    );

    if (FAILED(hr)) {
        blog(LOG_ERROR, "[obs-clean-output] D3D11CreateDevice on WinRT adapter failed: 0x%08X", hr);
        return false;
    }

    blog(LOG_INFO, "[obs-clean-output] D3D11OutputRenderer initialized on WinRT adapter (%ux%u, feature level 0x%X)", 
        width_, height_, static_cast<int>(out_level));
    return true;
}

bool D3D11OutputRenderer::BindScanoutTarget(HANDLE scanoutSharedHandle) {
    if (!scanoutSharedHandle || !device_) return false;

    scanout_texture_ = nullptr;

    // Try OpenSharedResource1 first (D3D11.1)
    winrt::com_ptr<ID3D11Device1> device1;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(device1.put()));
    if (SUCCEEDED(hr) && device1) {
        winrt::com_ptr<ID3D11Texture2D> tex;
        hr = device1->OpenSharedResource1(
            scanoutSharedHandle,
            IID_PPV_ARGS(tex.put())
        );
        if (SUCCEEDED(hr) && tex) {
            scanout_texture_ = tex;
            
            D3D11_TEXTURE2D_DESC desc;
            scanout_texture_->GetDesc(&desc);
            blog(LOG_INFO, "[obs-clean-output] Scanout texture bound via OpenSharedResource1: %ux%u, Format=%d, Usage=%d",
                desc.Width, desc.Height, desc.Format, desc.Usage);
            return true;
        }
        blog(LOG_WARNING, "[obs-clean-output] OpenSharedResource1 failed: 0x%08X, trying OpenSharedResource...", hr);
    }

    // Fallback to OpenSharedResource (D3D11.0)
    {
        winrt::com_ptr<ID3D11Texture2D> tex;
        hr = device_->OpenSharedResource(
            scanoutSharedHandle,
            IID_PPV_ARGS(tex.put())
        );
        if (SUCCEEDED(hr) && tex) {
            scanout_texture_ = tex;
            
            D3D11_TEXTURE2D_DESC desc;
            scanout_texture_->GetDesc(&desc);
            blog(LOG_INFO, "[obs-clean-output] Scanout texture bound via OpenSharedResource: %ux%u, Format=%d",
                desc.Width, desc.Height, desc.Format);
            return true;
        }
    }

    blog(LOG_ERROR, "[obs-clean-output] BindScanoutTarget failed: 0x%08X", hr);
    return false;
}

bool D3D11OutputRenderer::InitializeGPUSharedTexture() {
    if (!device_ || width_ == 0 || height_ == 0) return false;

    ID3D11Device *obs_device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!obs_device) {
        blog(LOG_ERROR, "[obs-clean-output] OBS primary D3D11 device unavailable.");
        return false;
    }

    winrt::com_ptr<ID3D11Device1> device1;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(device1.put()));
    if (FAILED(hr) || !device1) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (size_t i = 0; i < SHARED_BUFFER_COUNT; ++i) {
        winrt::com_ptr<ID3D11Texture2D> tex;
        hr = obs_device->CreateTexture2D(&desc, nullptr, tex.put());
        if (FAILED(hr) || !tex) {
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            hr = obs_device->CreateTexture2D(&desc, nullptr, tex.put());
            if (FAILED(hr) || !tex) return false;
        }

        obs_shared_textures_[i] = tex;

        if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
            winrt::com_ptr<IDXGIResource1> dxgi_res;
            hr = obs_shared_textures_[i]->QueryInterface(IID_PPV_ARGS(dxgi_res.put()));
            if (SUCCEEDED(hr) && dxgi_res) {
                dxgi_res->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &shared_handles_[i]
                );
            }
        } else {
            winrt::com_ptr<IDXGIResource> dxgi_res;
            hr = obs_shared_textures_[i]->QueryInterface(IID_PPV_ARGS(dxgi_res.put()));
            if (SUCCEEDED(hr) && dxgi_res) {
                dxgi_res->GetSharedHandle(&shared_handles_[i]);
            }
        }

        if (!shared_handles_[i]) return false;

        if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
            device1->OpenSharedResource1(shared_handles_[i], IID_PPV_ARGS(opened_shared_textures_[i].put()));
        } else {
            device_->OpenSharedResource(shared_handles_[i], IID_PPV_ARGS(opened_shared_textures_[i].put()));
        }

        if (!opened_shared_textures_[i]) return false;
    }

    producer_idx_ = 0;
    latest_produced_idx_.store(0);

    blog(LOG_INFO, "[obs-clean-output] Triple-Buffer Shared Surface Pool initialized! (%ux%u, 3 Buffers Zero-Stall)",
        width_, height_);
    return true;
}

bool D3D11OutputRenderer::ProducerCopyFrameGPU(gs_texture_t *obs_tex) {
    if (!obs_tex || !obs_shared_textures_[0]) return false;

    try {
        ID3D11Texture2D *src = static_cast<ID3D11Texture2D*>(gs_texture_get_obj(obs_tex));
        ID3D11Device *obs_device = static_cast<ID3D11Device*>(gs_get_device_obj());
        if (!src || !obs_device) return false;

        winrt::com_ptr<ID3D11DeviceContext> obs_context;
        obs_device->GetImmediateContext(obs_context.put());
        if (!obs_context) return false;

        uint32_t tex_w = gs_texture_get_width(obs_tex);
        uint32_t tex_h = gs_texture_get_height(obs_tex);
        uint32_t copy_w = (std::min)(tex_w, width_);
        uint32_t copy_h = (std::min)(tex_h, height_);

        D3D11_BOX box = {};
        box.right = copy_w;
        box.bottom = copy_h;
        box.back = 1;

        uint32_t cur_idx = producer_idx_;
        obs_context->CopySubresourceRegion(obs_shared_textures_[cur_idx].get(), 0, 0, 0, 0, src, 0, &box);

        latest_produced_idx_.store(cur_idx, std::memory_order_release);
        producer_idx_ = (producer_idx_ + 1) % SHARED_BUFFER_COUNT;
        return true;
    } catch (...) {
        return false;
    }
}

bool D3D11OutputRenderer::ConsumerPresentFrameGPU() {
    if (!context_ || !scanout_texture_) return false;

    try {
        uint32_t cur_idx = latest_produced_idx_.load(std::memory_order_acquire);
        if (cur_idx >= SHARED_BUFFER_COUNT || !opened_shared_textures_[cur_idx]) return false;

        context_->CopyResource(scanout_texture_.get(), opened_shared_textures_[cur_idx].get());
        context_->Flush();
        return true;
    } catch (...) {
        return false;
    }
}

bool D3D11OutputRenderer::CopyFrameToScanout(gs_texture_t *obs_tex) {
    if (!obs_tex || !device_ || !context_ || !scanout_texture_) {
        return false;
    }

    try {
        uint32_t tex_width = gs_texture_get_width(obs_tex);
        uint32_t tex_height = gs_texture_get_height(obs_tex);
        if (tex_width == 0 || tex_height == 0) return false;

        uint32_t copy_w = (std::min)(tex_width, width_);
        uint32_t copy_h = (std::min)(tex_height, height_);

        if (stagesurf_) {
            uint32_t surf_w = gs_stagesurface_get_width(stagesurf_);
            uint32_t surf_h = gs_stagesurface_get_height(stagesurf_);
            if (surf_w != copy_w || surf_h != copy_h) {
                gs_stagesurface_destroy(stagesurf_);
                stagesurf_ = nullptr;
            }
        }

        if (!stagesurf_) {
            stagesurf_ = gs_stagesurface_create(copy_w, copy_h, GS_BGRA);
            if (!stagesurf_) return false;
        }

        gs_stage_texture(stagesurf_, obs_tex);

        uint8_t *data = nullptr;
        uint32_t linesize = 0;
        if (!gs_stagesurface_map(stagesurf_, &data, &linesize) || !data) {
            return false;
        }

        D3D11_BOX box = {};
        box.right = copy_w;
        box.bottom = copy_h;
        box.back = 1;

        context_->UpdateSubresource(scanout_texture_.get(), 0, &box, data, linesize, 0);
        gs_stagesurface_unmap(stagesurf_);
        context_->Flush();

        return true;
    } catch (...) {
        return false;
    }
}
