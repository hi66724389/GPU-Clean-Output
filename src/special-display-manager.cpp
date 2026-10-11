#ifdef _WIN32
#include "special-display-manager.hpp"
#include <Windows.Devices.Display.Core.Interop.h>
#include <obs-module.h>
#include <iostream>

using namespace winrt;
using namespace winrt::Windows::Devices::Display;
using namespace winrt::Windows::Devices::Display::Core;
using namespace winrt::Windows::Graphics::DirectX;

SpecialDisplayManager::SpecialDisplayManager() {
    try {
        winrt::init_apartment();
    } catch (...) {
        // Apartment already initialized on this thread (e.g. Qt UI STA)
    }
}

SpecialDisplayManager::~SpecialDisplayManager() {
    Release();
}

std::vector<DisplayTargetInfo> SpecialDisplayManager::EnumerateSpecializedDisplays() {
    std::vector<DisplayTargetInfo> result;
    try {
        if (!display_manager_) {
            display_manager_ = DisplayManager::Create(DisplayManagerOptions::None);
        }
        auto targets = display_manager_.GetCurrentTargets();
        for (auto const& target : targets) {
            if (target.IsConnected() && target.UsageKind() == DisplayMonitorUsageKind::SpecialPurpose) {
                DisplayTargetInfo info;
                info.target = target;
                info.id = winrt::to_string(target.DeviceInterfacePath());

                try {
                    auto monitor = target.TryGetMonitor();
                    if (monitor) {
                        info.name = winrt::to_string(monitor.DisplayName());
                    } else {
                        info.name = "Specialized Display (" + info.id.substr(0, 12) + ")";
                    }
                } catch (...) {
                    info.name = "Specialized Display (" + info.id.substr(0, 12) + ")";
                }

                result.push_back(info);
            }
        }
    } catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] WinRT Exception in EnumerateSpecializedDisplays: %ls", ex.message().c_str());
    } catch (...) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Exception in EnumerateSpecializedDisplays");
    }
    return result;
}

bool SpecialDisplayManager::Initialize(const std::string &target_id) {
    try {
        blog(LOG_INFO, "[GPU-Clean-Output] Initializing DisplayManager...");
        if (!display_manager_) {
            display_manager_ = DisplayManager::Create(DisplayManagerOptions::None);
        }

        auto displays = EnumerateSpecializedDisplays();
        target_ = nullptr;

        if (displays.empty()) {
            blog(LOG_WARNING, "[GPU-Clean-Output] No Specialized Display Target found.");
            return false;
        }

        if (!target_id.empty() && target_id != "auto") {
            for (auto const& disp : displays) {
                if (disp.id == target_id) {
                    target_ = disp.target;
                    device_name_ = disp.name;
                    break;
                }
            }
        }

        // Fallback to first available target if not found or auto selected
        if (!target_) {
            target_ = displays[0].target;
            device_name_ = displays[0].name;
        }

        blog(LOG_INFO, "[GPU-Clean-Output] Selected Specialized Display Target: %s", device_name_.c_str());
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] WinRT Exception in Initialize: %ls (0x%08X)", 
            ex.message().c_str(), static_cast<uint32_t>(ex.code()));
        return false;
    }
    catch (const std::exception& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Standard Exception in Initialize: %s", ex.what());
        return false;
    }
}

bool SpecialDisplayManager::TryApplyAndLock(winrt::Windows::Graphics::DirectX::DirectXPixelFormat format) {
    if (!target_) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Cannot lock display: target is null.");
        return false;
    }

    try {
        blog(LOG_INFO, "[GPU-Clean-Output] Acquiring target state and locking display...");
        auto targetsList = winrt::single_threaded_vector<DisplayTarget>();
        targetsList.Append(target_);

        DisplayManagerResultWithState result = display_manager_.TryAcquireTargetsAndCreateEmptyState(targetsList);
        if (result.ErrorCode() != DisplayManagerResult::Success) {
            blog(LOG_ERROR, "[GPU-Clean-Output] TryAcquireTargetsAndCreateEmptyState failed with code: %d", 
                static_cast<int>(result.ErrorCode()));
            return false;
        }

        display_state_ = result.State();
        path_ = display_state_.ConnectTarget(target_);

        // Query available hardware display modes and apply preferred resolution & refresh rate
        try {
            auto modes = path_.FindModes(DisplayModeQueryOptions::OnlyPreferredResolution);
            if (modes.Size() == 0) {
                modes = path_.FindModes(DisplayModeQueryOptions::None);
            }
            if (modes.Size() > 0) {
                auto preferredMode = modes.GetAt(0);
                path_.ApplyPropertiesFromMode(preferredMode);
                blog(LOG_INFO, "[GPU-Clean-Output] Applied DisplayMode: %ux%u", 
                    preferredMode.SourceResolution().Width, preferredMode.SourceResolution().Height);
            }
        } catch (...) {
            blog(LOG_WARNING, "[GPU-Clean-Output] Could not query/apply explicit DisplayMode, using default.");
        }

        // Functionalize display state to ensure valid topology and wire format
        DisplayStateOperationResult funcResult = display_state_.TryFunctionalize(DisplayStateFunctionalizeOptions::None);
        if (funcResult.Status() != DisplayStateOperationStatus::Success) {
            blog(LOG_WARNING, "[GPU-Clean-Output] TryFunctionalize status: %d", static_cast<int>(funcResult.Status()));
        }

        // Create DisplayDevice for scanout operations
        display_device_ = display_manager_.CreateDisplayDevice(target_.Adapter());

        // Apply state to lock display exclusively and activate HDMI timing output
        DisplayStateOperationResult applyResult = display_state_.TryApply(DisplayStateApplyOptions::None);
        if (applyResult.Status() != DisplayStateOperationStatus::Success) {
            blog(LOG_ERROR, "[GPU-Clean-Output] TryApply failed with status: %d", 
                static_cast<int>(applyResult.Status()));
            return false;
        }

        auto mode = path_.SourceResolution();
        if (mode) {
            width_ = mode.Value().Width;
            height_ = mode.Value().Height;
        } else {
            width_ = 1920;
            height_ = 1080;
        }

        // Create Primary Surface for Hardware Direct Scanout using matched pixel format
        DisplayPrimaryDescription primaryDesc(
            width_,
            height_,
            format,
            DirectXColorSpace::RgbFullG22NoneP709,
            false,
            { 1, 0 }
        );

        surface_ = display_device_.CreatePrimary(target_, primaryDesc);
        DisplaySource scanout_source = display_device_.CreateScanoutSource(target_);
        scanout_ = display_device_.CreateSimpleScanout(scanout_source, surface_, 0, 1);
        task_pool_ = display_device_.CreateTaskPool();

        // Query IDisplayDeviceInterop to extract shared handle of DisplaySurface for D3D11 zero-copy interop
        winrt::com_ptr<IDisplayDeviceInterop> device_interop = display_device_.as<IDisplayDeviceInterop>();
        if (device_interop && surface_) {
            winrt::com_ptr<IInspectable> surface_inspectable = surface_.as<IInspectable>();
            HRESULT hr = device_interop->CreateSharedHandle(
                surface_inspectable.get(),
                nullptr,
                GENERIC_ALL,
                nullptr,
                &surface_shared_handle_
            );
            if (FAILED(hr)) {
                blog(LOG_ERROR, "[GPU-Clean-Output] CreateSharedHandle for DisplaySurface failed: 0x%08X", hr);
            } else {
                blog(LOG_INFO, "[GPU-Clean-Output] DisplaySurface shared handle created successfully.");
            }
        }

        blog(LOG_INFO, "[GPU-Clean-Output] Specialized display successfully locked & scanout pipeline ready! Resolution: %ux%u, Format: %d", 
            width_, height_, static_cast<int>(format));
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] WinRT Exception in TryApplyAndLock: %ls (0x%08X)", 
            ex.message().c_str(), static_cast<uint32_t>(ex.code()));
        return false;
    }
}

void SpecialDisplayManager::PresentScanout() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!task_pool_ || !scanout_) return;

    try {
        DisplayTask task = task_pool_.CreateTask();
        task.SetScanout(scanout_);
        task_pool_.ExecuteTask(task);
    } catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] WinRT Exception in PresentScanout: %ls (0x%08X)", 
            ex.message().c_str(), static_cast<uint32_t>(ex.code()));
    } catch (...) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Exception in PresentScanout");
    }
}

void SpecialDisplayManager::Release() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    
    // Turn off physical HDMI timing scanout by applying empty unattached DisplayState
    try {
        if (display_manager_ && target_) {
            auto targetsList = winrt::single_threaded_vector<DisplayTarget>();
            targetsList.Append(target_);
            DisplayManagerResultWithState result = display_manager_.TryAcquireTargetsAndCreateEmptyState(targetsList);
            if (result.ErrorCode() == DisplayManagerResult::Success) {
                auto emptyState = result.State();
                emptyState.TryApply(DisplayStateApplyOptions::None);
                blog(LOG_INFO, "[GPU-Clean-Output] Applied unattached state: HDMI signal turned OFF (No Signal).");
            }
        }
    } catch (...) {
        blog(LOG_WARNING, "[GPU-Clean-Output] Could not apply unattached state during release.");
    }

    if (surface_shared_handle_) {
        CloseHandle(surface_shared_handle_);
        surface_shared_handle_ = NULL;
    }

    try { task_ = nullptr; } catch (...) {}
    try { task_pool_ = nullptr; } catch (...) {}
    try { scanout_ = nullptr; } catch (...) {}
    try { surface_ = nullptr; } catch (...) {}
    try { path_ = nullptr; } catch (...) {}
    try { display_state_ = nullptr; } catch (...) {}
    try { display_device_ = nullptr; } catch (...) {}
    try { target_ = nullptr; } catch (...) {}
    try { display_manager_ = nullptr; } catch (...) {}
    
    dxgi_adapter_ = nullptr;
    dxgi_factory_ = nullptr;

    blog(LOG_INFO, "[GPU-Clean-Output] Specialized display released.");
}

IDXGIAdapter *SpecialDisplayManager::GetDXGIAdapter() {
    if (dxgi_adapter_) {
        return dxgi_adapter_.get();
    }

    #else

    #include "special-display-manager.hpp"
    #include <obs-module.h>
    #include <xf86drm.h>
    #include <xf86drmMode.h>
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <wayland-client.h>
    #include "drm-lease-v1-client-protocol.h"
    #include <algorithm>
    #include <cerrno>
    #include <cstring>
    #include <memory>

    struct SpecialDisplayManager::WaylandState {
        wl_display *display{nullptr};
        wl_registry *registry{nullptr};
        wp_drm_lease_device_v1 *device{nullptr};
        wp_drm_lease_connector_v1 *connector{nullptr};
        wp_drm_lease_v1 *lease{nullptr};
        int device_fd{-1};
        int lease_fd{-1};
        uint32_t connector_id{0};
        std::string name;
        std::string description;
        bool done{false};
        bool lease_done{false};
    };

    namespace {
    void registry_global(void *data, wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version)
    {
        auto *state = static_cast<SpecialDisplayManager::WaylandState *>(data);
        if (std::strcmp(interface, "wp_drm_lease_device_v1") == 0 && !state->device)
            state->device = static_cast<wp_drm_lease_device_v1 *>(
                wl_registry_bind(registry, name, &wp_drm_lease_device_v1_interface,
                                 std::min(version, 1u)));
    }
    void registry_remove(void *, wl_registry *, uint32_t) {}
    const wl_registry_listener registry_listener{registry_global, registry_remove};
    extern const wp_drm_lease_connector_v1_listener connector_listener;

    void drm_fd(void *data, wp_drm_lease_device_v1 *, int32_t fd)
    {
        auto *state = static_cast<SpecialDisplayManager::WaylandState *>(data);
        state->device_fd = fd;
    }
    void device_connector(void *data, wp_drm_lease_device_v1 *,
                          wp_drm_lease_connector_v1 *connector)
    {
        auto *state = static_cast<SpecialDisplayManager::WaylandState *>(data);
        if (!state->connector) {
            state->connector = connector;
            wp_drm_lease_connector_v1_add_listener(connector, &connector_listener, state);
        }
    }
    void device_done(void *data, wp_drm_lease_device_v1 *)
    {
        static_cast<SpecialDisplayManager::WaylandState *>(data)->done = true;
    }
    void device_released(void *, wp_drm_lease_device_v1 *) {}
    const wp_drm_lease_device_v1_listener device_listener{
        drm_fd, device_connector, device_done, device_released};

    void connector_name(void *data, wp_drm_lease_connector_v1 *, const char *name)
    {
        static_cast<SpecialDisplayManager::WaylandState *>(data)->name = name;
    }
    void connector_description(void *data, wp_drm_lease_connector_v1 *, const char *description)
    {
        static_cast<SpecialDisplayManager::WaylandState *>(data)->description = description;
    }
    void connector_id(void *data, wp_drm_lease_connector_v1 *, uint32_t id)
    {
        static_cast<SpecialDisplayManager::WaylandState *>(data)->connector_id = id;
    }
    void connector_done(void *, wp_drm_lease_connector_v1 *) {}
    void connector_withdrawn(void *, wp_drm_lease_connector_v1 *) {}
    const wp_drm_lease_connector_v1_listener connector_listener{
        connector_name, connector_description, connector_id, connector_done,
        connector_withdrawn};

    void lease_fd(void *data, wp_drm_lease_v1 *, int32_t fd)
    {
        auto *state = static_cast<SpecialDisplayManager::WaylandState *>(data);
        state->lease_fd = fd;
        state->lease_done = true;
    }
    void lease_finished(void *data, wp_drm_lease_v1 *)
    {
        static_cast<SpecialDisplayManager::WaylandState *>(data)->lease_done = true;
    }
    const wp_drm_lease_v1_listener lease_listener{lease_fd, lease_finished};
    }

    SpecialDisplayManager::SpecialDisplayManager() : wayland_(new WaylandState) {}

    SpecialDisplayManager::~SpecialDisplayManager()
    {
        Release();
        delete wayland_;
    }

    std::vector<DisplayTargetInfo> SpecialDisplayManager::EnumerateSpecializedDisplays()
    {
        std::vector<DisplayTargetInfo> result;
        auto *state = wayland_;
        if (!state->display)
            state->display = wl_display_connect(nullptr);
        if (!state->display)
            return result;
        if (!state->registry) {
            state->registry = wl_display_get_registry(state->display);
            wl_registry_add_listener(state->registry, &registry_listener, state);
        }
        wl_display_roundtrip(state->display);
        if (!state->device)
            return result;
        if (!state->connector) {
            wp_drm_lease_device_v1_add_listener(state->device, &device_listener, state);
            wl_display_roundtrip(state->display);
        }
        if (state->connector && state->connector_id != 0) {
            wl_display_roundtrip(state->display);
            DisplayTargetInfo info;
            info.id = "connector:" + std::to_string(state->connector_id);
            info.name = state->description.empty() ? state->name : state->description;
            if (info.name.empty())
                info.name = info.id;
            result.push_back(std::move(info));
        } else if (state->connector) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Wayland connector did not provide a DRM connector ID.");
        }
        return result;
    }

    bool SpecialDisplayManager::Initialize(const std::string &target_id)
    {
        auto displays = EnumerateSpecializedDisplays();
        if (displays.empty()) {
            blog(LOG_WARNING, "[GPU-Clean-Output] No DRM lease connectors advertised by Wayland.");
            return false;
        }
        if (!target_id.empty() && target_id != "auto" && target_id != displays[0].id)
            return false;
        target_id_ = displays[0].id;
        device_name_ = displays[0].name;
        return true;
    }

    bool SpecialDisplayManager::TryApplyAndLock(uint32_t width, uint32_t height,
                                                 uint32_t refresh_millihertz)
    {
        auto *state = wayland_;
        if (!state->device || !state->connector)
            return false;
        if (state->connector_id == 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Cannot acquire DRM lease: connector ID is zero.");
            return false;
        }
        if (width) width_ = width;
        if (height) height_ = height;
        if (refresh_millihertz) refresh_millihertz_ = refresh_millihertz;

        auto request = wp_drm_lease_device_v1_create_lease_request(state->device);
        wp_drm_lease_request_v1_request_connector(request, state->connector);
        state->lease_fd = -1;
        state->lease_done = false;
        state->lease = wp_drm_lease_request_v1_submit(request);
        wp_drm_lease_v1_add_listener(state->lease, &lease_listener, state);
        if (wl_display_roundtrip(state->display) < 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] DRM lease roundtrip failed: %s",
                 std::strerror(errno));
            wp_drm_lease_v1_destroy(state->lease);
            state->lease = nullptr;
            return false;
        }
        if (state->lease_fd < 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Wayland compositor denied DRM lease for connector:%u",
                 state->connector_id);
            wp_drm_lease_v1_destroy(state->lease);
            state->lease = nullptr;
            return false;
        }
        lease_fd_ = state->lease_fd;
        connector_id_ = state->connector_id;

        drmModeRes *resources = drmModeGetResources(lease_fd_);
        if (!resources) {
            blog(LOG_ERROR, "[GPU-Clean-Output] drmModeGetResources failed on lease fd: %s",
                 std::strerror(errno));
            return false;
        }
        drmModeConnector *connector = drmModeGetConnector(lease_fd_, connector_id_);
        if (!connector || connector->count_modes == 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Cannot query leased connector %u: %s",
                 connector_id_, connector ? "no modes available" : std::strerror(errno));
            if (connector) drmModeFreeConnector(connector);
            drmModeFreeResources(resources);
            return false;
        }
        drmModeModeInfo selected = connector->modes[0];
        uint64_t requested = static_cast<uint64_t>(refresh_millihertz_);
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < connector->count_modes; ++i) {
            const auto &mode = connector->modes[i];
            uint32_t refresh = mode.vrefresh > 0 ? static_cast<uint32_t>(mode.vrefresh * 1000) : 60000;
            uint64_t distance = (mode.hdisplay == width_ && mode.vdisplay == height_)
                ? (refresh > requested ? refresh - requested : requested - refresh) + 1
                : UINT64_MAX / 2;
            if (distance < best) {
                best = distance;
                selected = mode;
            }
        }
        width_ = selected.hdisplay;
        height_ = selected.vdisplay;
        refresh_millihertz_ = selected.vrefresh > 0 ? static_cast<uint32_t>(selected.vrefresh * 1000) : 60000;

        drmModeEncoder *encoder = connector->encoder_id
            ? drmModeGetEncoder(lease_fd_, connector->encoder_id) : nullptr;
        crtc_id_ = encoder ? encoder->crtc_id : (resources->count_crtcs ? resources->crtcs[0] : 0);
        if (encoder) drmModeFreeEncoder(encoder);
        framebuffer_pitch_ = width_ * 4;
        drm_mode_create_dumb create{width_, height_, 32, 0, 0, 0, 0};
        if (drmIoctl(lease_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0 ||
            drmModeAddFB(lease_fd_, width_, height_, 24, 32, framebuffer_pitch_,
                         create.handle, &framebuffer_id_) != 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Failed to create DRM framebuffer: %s",
                 std::strerror(errno));
            drmModeFreeConnector(connector);
            drmModeFreeResources(resources);
            return false;
        }
        dumb_handle_ = create.handle;
        dumb_size_ = create.size;
        drm_mode_map_dumb map{create.handle, 0};
        if (drmIoctl(lease_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] Failed to map DRM framebuffer: %s",
                 std::strerror(errno));
            return false;
        }
        framebuffer_ = static_cast<uint8_t *>(mmap(nullptr, dumb_size_, PROT_READ | PROT_WRITE,
                                                    MAP_SHARED, lease_fd_, map.offset));
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        if (framebuffer_ == MAP_FAILED) {
            framebuffer_ = nullptr;
            return false;
        }
        if (drmModeSetCrtc(lease_fd_, crtc_id_, framebuffer_id_, 0, 0, &connector_id_, 1,
                           &selected) != 0) {
            blog(LOG_ERROR, "[GPU-Clean-Output] drmModeSetCrtc failed: %s",
                 std::strerror(errno));
            return false;
        }
        return true;
    }

    void SpecialDisplayManager::PresentScanout()
    {
        // The leased CRTC scans the dumb buffer continuously. Frame updates become
        // visible after the producer has copied the new pixels into the mapping.
    }

    void SpecialDisplayManager::Release()
    {
        if (framebuffer_ && dumb_size_)
            munmap(framebuffer_, dumb_size_);
        if (lease_fd_ >= 0 && framebuffer_id_)
            drmModeRmFB(lease_fd_, framebuffer_id_);
        if (lease_fd_ >= 0 && dumb_handle_) {
            drm_mode_destroy_dumb destroy{};
            destroy.handle = dumb_handle_;
            drmIoctl(lease_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        if (lease_fd_ >= 0) close(lease_fd_);
        lease_fd_ = -1;
        framebuffer_ = nullptr;
        framebuffer_id_ = dumb_handle_ = 0;
        dumb_size_ = 0;
        if (wayland_ && wayland_->device_fd >= 0) {
            close(wayland_->device_fd);
            wayland_->device_fd = -1;
        }
        if (wayland_ && wayland_->lease) {
            wp_drm_lease_v1_destroy(wayland_->lease);
            wayland_->lease = nullptr;
        }
    }

    #endif

    #ifdef _WIN32

    if (!target_) {
        blog(LOG_ERROR, "[GPU-Clean-Output] GetDXGIAdapter: target_ is null");
        return nullptr;
    }

    try {
        auto adapter = target_.Adapter();
        auto adapterId = adapter.Id();
        LUID luid;
        luid.LowPart = adapterId.LowPart;
        luid.HighPart = adapterId.HighPart;

        blog(LOG_INFO, "[GPU-Clean-Output] WinRT Adapter LUID: %u:%d", luid.LowPart, luid.HighPart);

        if (!dxgi_factory_) {
            HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(dxgi_factory_.put()));
            if (FAILED(hr)) {
                blog(LOG_ERROR, "[GPU-Clean-Output] CreateDXGIFactory2 failed: 0x%08X", hr);
                return nullptr;
            }
        }

        HRESULT hr = dxgi_factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(dxgi_adapter_.put()));
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[GPU-Clean-Output] EnumAdapterByLuid failed: 0x%08X", hr);
            return nullptr;
        }

        DXGI_ADAPTER_DESC desc;
        dxgi_adapter_->GetDesc(&desc);
        blog(LOG_INFO, "[GPU-Clean-Output] Matched DXGI Adapter: %ls", desc.Description);

        return dxgi_adapter_.get();

    } catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[GPU-Clean-Output] WinRT error in GetDXGIAdapter: %ls", ex.message().c_str());
        return nullptr;
    } catch (...) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Exception in GetDXGIAdapter");
        return nullptr;
    }
}

#endif
