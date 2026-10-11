# GPU Clean Output Plugin (`GPU-Clean-Output`)

[English](README.md) | [正體中文](README.zh-TW.md)

**GPU-Clean-Output** is an advanced, high-performance OBS Studio output plugin for Windows 10/11 and Wayland Linux. It streams clean-feed video (Program or Preview) directly to physical HDMI/DisplayPort monitors configured as **Specialized Displays** ("Remove display from desktop") or leased through the Wayland DRM leasing protocol.

By bypassing the Windows Desktop Window Manager (DWM) compositor entirely, `GPU-Clean-Output` achieves zero-copy, hardware-exclusive direct scanout with broadcast-grade ultra-low latency.

---

## 🌟 Key Features

* **Hardware Direct Scanout (DWM Bypass):** Outputs directly to dedicated displays without going through DWM window compositing, eliminating OS windowing overhead and frame-jitter.
* **WinRT DisplayCore Integration:** Takes exclusive hardware ownership of monitors marked as "Specialized Display" in Windows.
* **GPU Zero-Copy Pipeline:** Passes OBS frame textures directly to DXGI swap chains on the GPU, avoiding CPU memory copies.
* **Multi-Output Support:** Configure up to **4 independent display output slots** simultaneously.
* **Flexible Video Routing:** Choose **Program (PGM)** or **Preview (PVW)** feed individually for each output slot (ideal for broadcast Studio Mode setup).
* **Ultra-Low Latency Engine:** Dedicated background worker threads with hybrid spin-wait mechanisms for sub-millisecond frame submission response.
* **Hot-Plug & Self-Recovery:** Automatic connection monitoring and recovery upon DXGI device resets or display topology changes.
* **Wayland DRM leasing:** On compositors advertising `wp_drm_lease_device_v1`, the selected connector is removed from desktop composition and driven through its leased DRM CRTC.
* **Explicit leased mode:** Each output slot has independent width, height, and refresh-rate controls. A value of `0` uses the connector's first advertised mode/default.

---

## 💻 Requirements & Prerequisites

* **Operating System:** Windows 10 (Version 1903 / Build 18362 or higher), Windows 11 (64-bit), or a Wayland Linux session with DRM leasing enabled.
  * ⚠️ **Edition Requirement:** Microsoft's "Specialized Displays" ("Remove display from desktop") feature is **ONLY supported on Windows Enterprise, Windows Pro for Workstations, and Windows IoT Enterprise** (Windows 10 / Windows 11).
  * ❌ **Unsupported Editions:** Standard **Windows Pro** and standard **Windows Home** editions are NOT supported (they lack the specialized display detachment setting).
* **OBS Studio:** OBS Studio v28.0.0 or higher (64-bit).
* **Hardware:** NVIDIA, AMD, or Intel GPU supporting DirectX 11.
* **Development / Build Tools:**
  * Visual Studio 2019 / 2022 (with *Desktop development with C++* workload).
  * CMake 3.16 or newer.
  * Windows 10/11 SDK.
  * Linux: CMake 3.16+, `libdrm`, `wayland-client`, `wayland-scanner`, and `wayland-protocols` (staging DRM lease protocol).

---

## 🔨 How to Build

Follow these steps to build `gpu-clean-output.dll` from source:

### 1. Clone the Repository
```powershell
git clone https://github.com/hi66724389/GPU-Clean-Output.git
cd GPU-Clean-Output
```

### 2. Configure with CMake
Open PowerShell or Developer Command Prompt for VS 2022 and create the build directory:
```powershell
mkdir build
cd build
```

If you have OBS Studio dependencies installed or extracted locally, specify the path via `OBS_SDK_DIR`:
```powershell
cmake -G "Visual Studio 17 2022" -A x64 -DOBS_SDK_DIR="C:/path/to/obs-studio-deps" ..
```
*(If `libobs` is already installed in standard CMake search paths, running `cmake -G "Visual Studio 17 2022" -A x64 ..` will auto-detect it.)*

### 3. Compile the Plugin
Build the Release binary using CMake:
```powershell
cmake --build . --config Release
```

Upon successful compilation, the plugin DLL will be located at:
`build/Release/gpu-clean-output.dll` (or `build/gpu-clean-output.dll`).

On Linux, configure and build with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Flatpak OBS builds

The project includes `flatpak/com.obsproject.Studio.Plugin.GPUCleanOutput.yml`
for the OBS Studio Flatpak runtime. It builds as an OBS plugin extension and
uses the matching OBS Studio source tag for headers while linking against the
libraries supplied by the OBS runtime. The CMake install rule places the
module in the extension at
`/app/plugins/GPUCleanOutput/lib/obs-plugins`.

When building from an OBS Flatpak SDK shell:

```bash
flatpak-builder --force-clean flatpak-build \
  flatpak/com.obsproject.Studio.Plugin.GPUCleanOutput.yml
```

The manifest currently follows OBS Studio 32.2.2. Update its OBS source tag
when targeting a different OBS runtime version. The runtime must provide the
staging `drm-lease-v1` protocol through `wayland-protocols`.

---

## 📦 How to Install `.dll` File

1. Close OBS Studio if it is currently running.
2. Locate the compiled `gpu-clean-output.dll` file.
3. Copy `gpu-clean-output.dll` into your OBS Studio plugins directory:
   * **Standard Installation (64-bit):**
     `C:\Program Files\obs-studio\obs-plugins\64bit\`
   * **Portable OBS Studio Installation:**
     `<OBS_DIRECTORY>\obs-plugins\64bit\`
4. Launch OBS Studio.
5. Verify installation by checking the OBS top menu bar: **Tools** -> **`GPU Clean Output (Specialized Display)`**.

---

## 🖥️ How to Trigger Screen Exclusivity (Specialized Display & Direct Scanout)

To trigger hardware-exclusive direct scanout, you must first configure the target monitor in Windows and then activate the plugin in OBS Studio.

### Step 1: Configure Windows "Specialized Display" Mode

Mark your external broadcast/monitor display as a Specialized Display to remove it from desktop DWM control:

1. Connect your target HDMI/DisplayPort monitor to your PC.
2. Open **Windows Settings** (`Win + I`).
3. Navigate to **System** -> **Display** -> **Advanced display settings** (or **Advanced display** in Windows 11).
4. In the display dropdown at the top, select your target monitor.
5. Find the **"Remove display from desktop"** option (or **Specialized display** toggle) and turn it **ON**.
6. Windows will immediately detach this monitor from the desktop space (it will no longer display the Windows desktop background or cursor).

> ⚠️ **Important Edition Notice:** If the **"Remove display from desktop" toggle is missing** in Advanced Display settings, check your Windows edition. This setting requires **Windows Enterprise, Windows Pro for Workstations, or Windows IoT Enterprise**. Standard **Windows Pro** and **Windows Home** editions do not support this feature.
>
> ℹ️ **Note:** The monitor will appear black or go to sleep until `GPU-Clean-Output` takes ownership and begins streaming.

---

### Step 2: Activate Direct Output in OBS Studio

1. Open **OBS Studio**.
2. Go to the top menu bar and click **Tools** -> **`GPU Clean Output (Specialized Display)`**.
3. In the plugin configuration window:
   * Check **Enable Direct Output (啟動硬體直輸出)** to turn on master output.
   * **Target Specialized Display 1**: Select your display target from the dropdown list (e.g. `Auto-Detect First Specialized Display` or the specific hardware ID of your monitor).
   * **Source Output 1**: Select either **Program (PGM 主畫面)** or **Preview (PVW 預覽畫面)**.
   * *(Optional)* Configure Output Slots 2 to 4 if you have multiple specialized displays connected.
4. Click **OK** / close the dialog.

The targeted physical display will instantly wake up and receive the ultra-low latency clean feed direct from the GPU!

### Wayland DRM lease mode

On Linux, use a compositor that advertises `wp_drm_lease_device_v1` (for example, a compositor configured for VR/HDR or direct-display leasing). The target list shows the connectors offered by the compositor. Select a connector, then set **Lease width**, **Lease height**, and **Lease refresh rate** for that output slot; set any value to `0` to let the backend choose the connector default. When the output starts, the connector is leased away from the desktop and the OBS feed is written to the leased DRM framebuffer. Releasing the output returns the connector to the compositor.

---

## 🛠️ Architecture & Technical Summary

```
[ OBS Render Engine ] ---> (Zero-Copy GPU Texture) ---> [ Direct3D 11 Renderer ]
                                                                 |
                                                     DXGI SwapChain Present
                                                                 v
[ WinRT DisplayTarget ] <--- (DisplayCore API) <--- [ Exclusive Physical Display ]
```

* **API Layer:** WinRT `Windows.Devices.Display.Core::DisplayManager` for hardware target acquisition, locking, and display state management.
* **Rendering Engine:** Dedicated D3D11 device bound to the specific physical GPU adapter (LUID) connected to the output target.
* **Render Pipeline:** Sub-millisecond queue flushing with zero-copy texture sharing.

---

## 📄 License

This project is licensed under the GPL-2.0 License - see the [LICENSE](LICENSE) file for details.
