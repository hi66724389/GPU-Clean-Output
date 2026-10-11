#pragma once

#ifdef _WIN32

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <mutex>


struct DisplayTargetInfo {
    std::string id;
    std::string name;
    winrt::Windows::Devices::Display::Core::DisplayTarget target{ nullptr };
};

class SpecialDisplayManager {
public:
    struct WaylandState;
    SpecialDisplayManager();
    ~SpecialDisplayManager();

    // Enumerate all specialized displays available on the system
    std::vector<DisplayTargetInfo> EnumerateSpecializedDisplays();

    // Initializes WinRT DisplayManager and targets a specific specialized display by ID (or auto if empty)
    bool Initialize(const std::string &target_id = "");

    // Acquires and locks the specialized display target and creates hardware scanout
    bool TryApplyAndLock(
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat format = 
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized
    );


    // Submit frame task to hardware scanout
    void PresentScanout();

    // Releases display ownership
    void Release();

    // Getters for downstream modules
    bool HasSpecializedDisplay() const { return target_ != nullptr; }
    winrt::Windows::Devices::Display::Core::DisplayTarget GetTarget() const { return target_; }
    winrt::Windows::Devices::Display::Core::DisplayDevice GetDisplayDevice() const { return display_device_; }
    winrt::Windows::Devices::Display::Core::DisplayState GetDisplayState() const { return display_state_; }
    HANDLE GetSurfaceSharedHandle() const { return surface_shared_handle_; }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    std::string GetDeviceName() const { return device_name_; }

    // Get DXGI adapter matching the WinRT display target's GPU
    IDXGIAdapter *GetDXGIAdapter();

private:
    winrt::Windows::Devices::Display::Core::DisplayManager display_manager_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayTarget target_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayDevice display_device_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayState display_state_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayPath path_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplaySurface surface_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayScanout scanout_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayTaskPool task_pool_{ nullptr };
    winrt::Windows::Devices::Display::Core::DisplayTask task_{ nullptr };
    HANDLE surface_shared_handle_{ NULL };
    winrt::com_ptr<IDXGIFactory4> dxgi_factory_;
    winrt::com_ptr<IDXGIAdapter> dxgi_adapter_;
    mutable std::mutex render_mutex_;


    uint32_t width_{ 1920 };
    uint32_t height_{ 1080 };
    std::string device_name_;
};

#else

#include <cstdint>
#include <string>
#include <vector>

struct DisplayModeInfo {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refresh_millihertz{0};
    std::string name;
};

struct DisplayTargetInfo {
    std::string id;
    std::string name;
    std::vector<DisplayModeInfo> modes;
};

class SpecialDisplayManager {
public:
    struct WaylandState;
    SpecialDisplayManager();
    ~SpecialDisplayManager();
    std::vector<DisplayTargetInfo> EnumerateSpecializedDisplays();
    bool Initialize(const std::string &target_id = "");
    bool TryApplyAndLock(uint32_t width = 0, uint32_t height = 0,
                         uint32_t refresh_millihertz = 0);
    void PresentScanout();
    void Release();
    bool HasSpecializedDisplay() const { return lease_fd_ >= 0; }
    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetRefreshMillihertz() const { return refresh_millihertz_; }
    const std::string &GetDeviceName() const { return device_name_; }
    int GetLeaseFd() const { return lease_fd_; }
    uint8_t *GetFrameBuffer() const { return framebuffer_; }
    uint32_t GetFrameBufferPitch() const { return framebuffer_pitch_; }

private:
    WaylandState *wayland_{nullptr};
    int lease_fd_{-1};
    uint32_t connector_id_{0};
    uint32_t crtc_id_{0};
    uint32_t framebuffer_id_{0};
    uint32_t dumb_handle_{0};
    uint64_t dumb_size_{0};
    uint8_t *framebuffer_{nullptr};
    uint32_t framebuffer_pitch_{0};
    uint32_t width_{1920};
    uint32_t height_{1080};
    uint32_t refresh_millihertz_{60000};
    std::string target_id_;
    std::string device_name_;
};

#endif
