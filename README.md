# OBS Clean Output Plugin (`obs-clean-output`)

[English](README.md) | [正體中文](README.zh-TW.md)

An advanced, high-performance OBS Studio output plugin for Windows 10/11. It streams clean-feed video (Program or Preview) directly to physical HDMI/DisplayPort monitors configured as **Specialized Displays** ("Remove display from desktop") using DirectX 11 and WinRT `Windows.Devices.Display.Core`.

By bypassing the Windows Desktop Window Manager (DWM) compositor entirely, `obs-clean-output` achieves zero-copy, hardware-exclusive direct scanout with broadcast-grade ultra-low latency.

---

## 🌟 Key Features

* **Hardware Direct Scanout (DWM Bypass):** Outputs directly to dedicated displays without going through DWM window compositing, eliminating OS windowing overhead and frame-jitter.
* **WinRT DisplayCore Integration:** Takes exclusive hardware ownership of monitors marked as "Specialized Display" in Windows.
* **GPU Zero-Copy Pipeline:** Passes OBS frame textures directly to DXGI swap chains on the GPU, avoiding CPU memory copies.
* **Multi-Output Support:** Configure up to **4 independent display output slots** simultaneously.
* **Flexible Video Routing:** Choose **Program (PGM)** or **Preview (PVW)** feed individually for each output slot (ideal for broadcast Studio Mode setup).
* **Ultra-Low Latency Engine:** Dedicated background worker threads with hybrid spin-wait mechanisms for sub-millisecond frame submission response.
* **Hot-Plug & Self-Recovery:** Automatic connection monitoring and recovery upon DXGI device resets or display topology changes.

---

## 💻 Requirements & Prerequisites

* **Operating System:** Windows 10 (Version 1903 / Build 18362 or higher) or Windows 11 (64-bit).
* **OBS Studio:** OBS Studio v28.0.0 or higher (64-bit).
* **Hardware:** NVIDIA, AMD, or Intel GPU supporting DirectX 11.
* **Development / Build Tools:**
  * Visual Studio 2019 / 2022 (with *Desktop development with C++* workload).
  * CMake 3.16 or newer.
  * Windows 10/11 SDK.

---

## 🔨 How to Build

Follow these steps to build `obs-clean-output.dll` from source:

### 1. Clone the Repository
```powershell
git clone https://github.com/your-username/obs-clean-output.git
cd obs-clean-output
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
`build/Release/obs-clean-output.dll` (or `build/obs-clean-output.dll`).

---

## 📦 How to Install `.dll` File

1. Close OBS Studio if it is currently running.
2. Locate the compiled `obs-clean-output.dll` file.
3. Copy `obs-clean-output.dll` into your OBS Studio plugins directory:
   * **Standard Installation (64-bit):**
     `C:\Program Files\obs-studio\obs-plugins\64bit\`
   * **Portable OBS Studio Installation:**
     `<OBS_DIRECTORY>\obs-plugins\64bit\`
4. Launch OBS Studio.
5. Verify installation by checking the OBS top menu bar: **Tools** -> **`D3D11 Specialized Display Clean Output`**.

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

> ℹ️ **Note:** The monitor will appear black or go to sleep until `obs-clean-output` takes ownership and begins streaming.

---

### Step 2: Activate Direct Output in OBS Studio

1. Open **OBS Studio**.
2. Go to the top menu bar and click **Tools** -> **`D3D11 Specialized Display Clean Output`**.
3. In the plugin configuration window:
   * Check **Enable Direct Output (啟動硬體直輸出)** to turn on master output.
   * **Target Specialized Display 1**: Select your display target from the dropdown list (e.g. `Auto-Detect First Specialized Display` or the specific hardware ID of your monitor).
   * **Source Output 1**: Select either **Program (PGM 主畫面)** or **Preview (PVW 預覽畫面)**.
   * *(Optional)* Configure Output Slots 2 to 4 if you have multiple specialized displays connected.
4. Click **OK** / close the dialog.

The targeted physical display will instantly wake up and receive the ultra-low latency clean feed direct from the GPU!

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

This project is licensed under the GPL-2.0 License - see the LICENSE file for details.
