---
name: obs-specialized-display-direct-scanout
description: Technical skill guide for implementing WinRT DisplayCore direct hardware scanout in OBS Studio using D3D11 cross-adapter staging and exclusive display state management.
---

# WinRT DisplayCore Direct Hardware Scanout for OBS Studio

This skill provides step-by-step patterns, anti-patterns, and architectural principles for outputting OBS Studio video frames directly to Windows Specialized Displays ("Remove display from desktop") using WinRT `Windows.Devices.Display.Core` and DirectX 11.

> ⚠️ **Operating System Edition Prerequisite:**
> Microsoft's "Specialized Displays" ("Remove display from desktop") functionality is strictly supported only on **Windows Enterprise, Windows Pro for Workstations, and Windows IoT Enterprise (Windows 10/11 64-bit)**. Standard Windows Pro and standard Windows Home editions are NOT supported as they lack the OS-level display detachment capability.

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

### Pattern 5: Source Active/Showing Refcounts for Offscreen Preview Rendering
- **Rule**: OBS video engine pauses frame updates and ticks for scenes/sources whose showing/active refcounts are 0. When rendering preview (PVW) or offscreen scenes, you MUST increment active and showing counters on the target source:
  ```cpp
  obs_source_inc_showing(pvw_source);
  obs_source_inc_active(pvw_source);
  // ... render ...
  obs_source_dec_active(old_source);
  obs_source_dec_showing(old_source);
  ```

### Pattern 6: Mandatory `gs_texrender_reset` for Texture Render Loop
- **Rule**: `gs_texrender_end` sets `texrender->rendered = true`. Subsequent calls to `gs_texrender_begin` WILL FAIL unless `gs_texrender_reset` is called prior to `gs_texrender_begin` every frame:
  ```cpp
  gs_texrender_reset(pvw_texrender_);
  if (gs_texrender_begin(pvw_texrender_, width, height)) {
      // Draw operations
      gs_texrender_end(pvw_texrender_);
  }
  ```

### Pattern 8: GPU Zero-Copy via DXGI Cross-Adapter Shared Surface
- **Rule**: Never use CPU staging buffers (`gs_stagesurf`) and `memcpy` for video frame transfer. Create a shared `ID3D11Texture2D` on OBS's primary D3D11 device with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` (or `D3D11_RESOURCE_MISC_SHARED`), export the handle via `CreateSharedHandle`, and open it on the dedicated WinRT scanout device (`OpenSharedResource1`).
- **Implementation**:
  ```cpp
  // Producer (OBS Render Thread): Copy frame to shared texture on GPU
  obs_context->CopySubresourceRegion(obs_shared_texture.get(), 0, 0, 0, 0, src, 0, &box);

  // Consumer (Scanout Worker Thread): Copy from shared texture on dedicated GPU device
  context_->CopyResource(scanout_texture_.get(), opened_shared_texture_.get());
  context_->Flush();
  ```

### Pattern 9: Producer-Consumer Thread Queue for Asynchronous Display Scanout
- **Rule**: NEVER execute `context->Flush()` or `PresentScanout()` directly inside OBS's graphics render callback (`obs_add_main_rendered_callback`). Launch a background worker thread (`std::thread`) per display slot.
- **Producer (OBS GS Render Thread)**: Perform GPU-to-GPU copy to shared surface, increment pending frame counter, notify condition variable (`queue_cv.notify_one()`), and return immediately to avoid blocking OBS rendering.
- **Consumer (Worker Thread)**: Wait on condition variable, dequeue frame signal, acquire Keyed Mutex, copy to scanout texture, call `context_->Flush()`, and present to WinRT `DisplaySurface`.

### Pattern 10: Fault-Tolerant Hot-Plug & Auto-Recovery State Machine
- **Rule**: Catch DXGI device removal (`device_->GetDeviceRemovedReason()`) and WinRT COM exceptions during scanout presentation. When a physical specialized monitor is hot-unplugged or display driver resets:
  1. Set slot state `is_degraded = true`.
  2. Perform graceful pipeline teardown (`StopPipelineInternal()`).
  3. Periodically poll for device re-attachment (every 2 seconds) in render loop without crashing OBS.
  4. Automatically re-initialize D3D11 device and WinRT `DisplaySurface` when physical display is re-plugged.

### Pattern 11: Sub-Millisecond Ultra-Low Latency (< 0.5ms) Pipeline
- **Rule**: To achieve sub-millisecond rendering latency without thread stalls or OS context switch delays:
  1. **Triple-Buffered Shared Surface Pool**: Maintain a 3-buffer shared texture ring (`obs_shared_textures_[3]`). Producer and Consumer operate on alternating indices via atomic `std::memory_order_release` / `acquire`, eliminating Keyed Mutex GPU wait stalls.
  2. **Hybrid Microsecond Spin-Wait**: In the Consumer background thread, spin-yield for 500 iterations checking for new frames before putting thread to sleep. Reduces thread wakeup latency from ~1.5ms to **< 0.05ms (50µs)**!

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
| Omitting `obs_source_inc_showing/active` on PVW | Preview scene video/animation freezes on first frame | Increment `inc_showing` and `inc_active` while preview source is selected |
| Omitting `gs_texrender_reset` | `gs_texrender_begin` fails from frame 2; falls back silently to PGM | Call `gs_texrender_reset` before `gs_texrender_begin` every frame |
| Omitting `gs_ortho` after `gs_texrender_begin` | Scene renders off-screen / scaled to single pixel | Explicitly call `gs_ortho(0, w, 0, h, -100, 100)` inside texrender pass |
| Using CPU `memcpy` for scanout transfer | High CPU utilization & PCIe transfer latency | Use DXGI cross-adapter shared surface (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) |
| Blocking OBS render thread with `Flush()` | Micro-stutters and locked frame drops in OBS | Move `Flush()` and `PresentScanout()` to a dedicated Consumer worker thread |
| Deploying on standard Windows Pro / Home | "Remove display from desktop" toggle missing in Settings; 0 specialized targets found | Use Windows Enterprise, Pro for Workstations, or IoT Enterprise edition |

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
