---
name: obs-specialized-display-direct-scanout
description: Technical skill guide for implementing WinRT DisplayCore direct hardware scanout in OBS Studio using D3D11 cross-adapter staging and exclusive display state management.
---

# WinRT DisplayCore Direct Hardware Scanout for OBS Studio

This skill provides step-by-step patterns, anti-patterns, and architectural principles for outputting OBS Studio video frames directly to Windows Specialized Displays ("Remove display from desktop") using WinRT `Windows.Devices.Display.Core` and DirectX 11.

---

## 1. Core Architecture

```
[ OBS Render Engine ] ---> (gs_stage_texture) ---> [ CPU Staging Buffer ]
                                                            |
                                                   memcpy (RowPitch)
                                                            v
[ WinRT DisplayTarget GPU Adapter ] ---> [ Dedicated Secondary D3D11 Device ]
                                                            |
                                                 UpdateSubresource + Flush()
                                                            v
                                            [ WinRT DisplaySurface Scanout ]
```

---

## 2. Key Rules & Implementation Patterns

### Pattern 1: Match D3D11 Device to WinRT Adapter via GPU LUID
- **Rule**: Do NOT use OBS's primary D3D11 Device to open WinRT `DisplaySurface` shared handles directly. WinRT `DisplaySurface` is owned by the specific GPU adapter connected to the physical display target.
- **Implementation**:
  1. Retrieve LUID: `winrt::Windows::Graphics::DisplayAdapterId id = target.Adapter().Id();`
  2. Map to DXGI: `IDXGIFactory4::EnumAdapterByLuid(luid, IID_PPV_ARGS(&dxgi_adapter))`
  3. Create dedicated D3D11 Device: `D3D11CreateDevice(dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, ...)`
  4. Open Shared Resource: `device1->OpenSharedResource1(sharedHandle, ...)`

### Pattern 2: Thread-Safe OBS Graphics Context Management
- **Rule**: Never invoke OBS graphics functions (`obs_get_main_texture`, `gs_texture_get_color_format`) on UI or non-render threads without acquiring the thread-local graphics context.
- **Implementation**:
  ```cpp
  obs_enter_graphics();
  // Call GS / OBS texture functions
  obs_leave_graphics();
  ```

### Pattern 3: Isolated Command Queue Flushing
- **Rule**: NEVER call `Flush()` on OBS's native D3D11 context (causes editor blackout). Always call `context->Flush()` on your **dedicated secondary D3D11 device context** after `UpdateSubresource` to push frame data to physical hardware scanout immediately.

### Pattern 4: Orderly Resource Teardown & Unattached DisplayState Restore
- **Rule**: Teardown DirectX references before closing handles. Apply an empty `DisplayState` to turn off physical HDMI clock output when closing or disabling output.
- **Teardown Sequence**:
  1. Unhook render callbacks (`obs_remove_main_rendered_callback`).
  2. Release D3D11 scanout texture (`scanout_texture_ = nullptr`).
  3. Close shared handle (`CloseHandle(surface_shared_handle_)`).
  4. Apply empty unattached `DisplayState`:
     ```cpp
     auto emptyState = display_manager.TryAcquireTargetsAndCreateEmptyState({ target }).State();
     emptyState.TryApply(DisplayStateApplyOptions::None);
     ```
  5. Isolated WinRT COM smart pointer release wrapped in individual `try { ... } catch (...) {}` blocks.

---

## 3. Anti-Patterns (Common Pitfalls)

| Anti-Pattern | Failure Mode | Correct Pattern |
| :--- | :--- | :--- |
| Hardcoding `B8G8R8A8` pixel format | All-black screen when source is `R8G8B8A8` | Dynamically map source `gs_color_format` or use standard staging conversion |
| Calling `context->Flush()` on OBS device | PGM / PVW editor blackout | Flush only on isolated secondary D3D11 device |
| Missing `context->Flush()` on secondary device | Signal active but black / frame updates only on stop | Flush secondary D3D11 device context after `UpdateSubresource` |
| Closing shared handle while D3D11 texture active | D3D11 kernel driver exception on uncheck | Release D3D11 texture pointer BEFORE closing NT handle |
| Leaving `DisplayState` active on teardown | Monitor remains on / frozen on last frame | Apply empty unattached `DisplayState` during `Release()` |
| Omission of `/utf-8` MSVC flag | Mojibake / corrupt Traditional Chinese text | Add `add_compile_options(/utf-8)` in `CMakeLists.txt` |

---

## 4. CMake Setup Checklist

```cmake
cmake_minimum_required(VERSION 3.18)
project(obs-clean-output LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

# Link C++/WinRT and DirectX libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    d3d11
    dxgi
    WindowsApp
)
```
