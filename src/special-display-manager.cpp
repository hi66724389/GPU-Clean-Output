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
        blog(LOG_ERROR, "[obs-clean-output] WinRT Exception in EnumerateSpecializedDisplays: %ls", ex.message().c_str());
    } catch (...) {
        blog(LOG_ERROR, "[obs-clean-output] Exception in EnumerateSpecializedDisplays");
    }
    return result;
}

bool SpecialDisplayManager::Initialize(const std::string &target_id) {
    try {
        blog(LOG_INFO, "[obs-clean-output] Initializing DisplayManager...");
        if (!display_manager_) {
            display_manager_ = DisplayManager::Create(DisplayManagerOptions::None);
        }

        auto displays = EnumerateSpecializedDisplays();
        target_ = nullptr;

        if (displays.empty()) {
            blog(LOG_WARNING, "[obs-clean-output] No Specialized Display Target found.");
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

        blog(LOG_INFO, "[obs-clean-output] Selected Specialized Display Target: %s", device_name_.c_str());
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[obs-clean-output] WinRT Exception in Initialize: %ls (0x%08X)", 
            ex.message().c_str(), static_cast<uint32_t>(ex.code()));
        return false;
    }
    catch (const std::exception& ex) {
        blog(LOG_ERROR, "[obs-clean-output] Standard Exception in Initialize: %s", ex.what());
        return false;
    }
}

bool SpecialDisplayManager::TryApplyAndLock(winrt::Windows::Graphics::DirectX::DirectXPixelFormat format) {
    if (!target_) {
        blog(LOG_ERROR, "[obs-clean-output] Cannot lock display: target is null.");
        return false;
    }

    try {
        blog(LOG_INFO, "[obs-clean-output] Acquiring target state and locking display...");
        auto targetsList = winrt::single_threaded_vector<DisplayTarget>();
        targetsList.Append(target_);

        DisplayManagerResultWithState result = display_manager_.TryAcquireTargetsAndCreateEmptyState(targetsList);
        if (result.ErrorCode() != DisplayManagerResult::Success) {
            blog(LOG_ERROR, "[obs-clean-output] TryAcquireTargetsAndCreateEmptyState failed with code: %d", 
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
                blog(LOG_INFO, "[obs-clean-output] Applied DisplayMode: %ux%u", 
                    preferredMode.SourceResolution().Width, preferredMode.SourceResolution().Height);
            }
        } catch (...) {
            blog(LOG_WARNING, "[obs-clean-output] Could not query/apply explicit DisplayMode, using default.");
        }

        // Functionalize display state to ensure valid topology and wire format
        DisplayStateOperationResult funcResult = display_state_.TryFunctionalize(DisplayStateFunctionalizeOptions::None);
        if (funcResult.Status() != DisplayStateOperationStatus::Success) {
            blog(LOG_WARNING, "[obs-clean-output] TryFunctionalize status: %d", static_cast<int>(funcResult.Status()));
        }

        // Create DisplayDevice for scanout operations
        display_device_ = display_manager_.CreateDisplayDevice(target_.Adapter());

        // Apply state to lock display exclusively and activate HDMI timing output
        DisplayStateOperationResult applyResult = display_state_.TryApply(DisplayStateApplyOptions::None);
        if (applyResult.Status() != DisplayStateOperationStatus::Success) {
            blog(LOG_ERROR, "[obs-clean-output] TryApply failed with status: %d", 
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
                blog(LOG_ERROR, "[obs-clean-output] CreateSharedHandle for DisplaySurface failed: 0x%08X", hr);
            } else {
                blog(LOG_INFO, "[obs-clean-output] DisplaySurface shared handle created successfully.");
            }
        }

        blog(LOG_INFO, "[obs-clean-output] Specialized display successfully locked & scanout pipeline ready! Resolution: %ux%u, Format: %d", 
            width_, height_, static_cast<int>(format));
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[obs-clean-output] WinRT Exception in TryApplyAndLock: %ls (0x%08X)", 
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
        blog(LOG_ERROR, "[obs-clean-output] WinRT Exception in PresentScanout: %ls (0x%08X)", 
            ex.message().c_str(), static_cast<uint32_t>(ex.code()));
    } catch (...) {
        blog(LOG_ERROR, "[obs-clean-output] Exception in PresentScanout");
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
                blog(LOG_INFO, "[obs-clean-output] Applied unattached state: HDMI signal turned OFF (No Signal).");
            }
        }
    } catch (...) {
        blog(LOG_WARNING, "[obs-clean-output] Could not apply unattached state during release.");
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

    blog(LOG_INFO, "[obs-clean-output] Specialized display released.");
}

IDXGIAdapter *SpecialDisplayManager::GetDXGIAdapter() {
    if (dxgi_adapter_) {
        return dxgi_adapter_.get();
    }

    if (!target_) {
        blog(LOG_ERROR, "[obs-clean-output] GetDXGIAdapter: target_ is null");
        return nullptr;
    }

    try {
        auto adapter = target_.Adapter();
        auto adapterId = adapter.Id();
        LUID luid;
        luid.LowPart = adapterId.LowPart;
        luid.HighPart = adapterId.HighPart;

        blog(LOG_INFO, "[obs-clean-output] WinRT Adapter LUID: %u:%d", luid.LowPart, luid.HighPart);

        if (!dxgi_factory_) {
            HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(dxgi_factory_.put()));
            if (FAILED(hr)) {
                blog(LOG_ERROR, "[obs-clean-output] CreateDXGIFactory2 failed: 0x%08X", hr);
                return nullptr;
            }
        }

        HRESULT hr = dxgi_factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(dxgi_adapter_.put()));
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[obs-clean-output] EnumAdapterByLuid failed: 0x%08X", hr);
            return nullptr;
        }

        DXGI_ADAPTER_DESC desc;
        dxgi_adapter_->GetDesc(&desc);
        blog(LOG_INFO, "[obs-clean-output] Matched DXGI Adapter: %ls", desc.Description);

        return dxgi_adapter_.get();

    } catch (const winrt::hresult_error& ex) {
        blog(LOG_ERROR, "[obs-clean-output] WinRT error in GetDXGIAdapter: %ls", ex.message().c_str());
        return nullptr;
    } catch (...) {
        blog(LOG_ERROR, "[obs-clean-output] Exception in GetDXGIAdapter");
        return nullptr;
    }
}
