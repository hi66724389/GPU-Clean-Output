#pragma once

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
