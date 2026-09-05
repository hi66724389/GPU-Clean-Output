# GPU Clean Output 外掛 (`GPU-Clean-Output`)

[English](README.md) | [正體中文](README.zh-TW.md)

`GPU-Clean-Output` 是一款專為 Windows 10/11 設計的高效能 OBS Studio 畫面輸出外掛。透過 DirectX 11 與 WinRT `Windows.Devices.Display.Core` API，本外掛能將 OBS 的無干擾純淨畫面（Program 主畫面或 Preview 預覽畫面）直接傳送至設定為 **「特殊用途顯示器 (Specialized Display)」**（即從桌面移除）的實體 HDMI/DisplayPort 顯示器。

透過完全繞過 Windows 桌面視窗管理器 (DWM) 的畫面合成機制，`GPU-Clean-Output` 實現了 GPU 零拷貝 (Zero-Copy) 與硬體獨佔直輸出 (Direct Scanout)，提供廣播級的亞毫秒超低延遲與極致順暢度。

---

## 🌟 主要特色 (Key Features)

* **硬體直輸出 (DWM Bypass)：** 繞過 Windows DWM 視窗合成器，消除 OS 視窗管理開銷與畫面抖動，實現真正的硬體直連輸出。
* **WinRT DisplayCore 整合：** 深度整合 Windows DisplayCore API，獲得 marked 為「特殊用途顯示器」的顯示器硬體獨佔控制權。
* **GPU 零拷貝管道 (Zero-Copy Pipeline)：** 將 OBS 渲染貼圖直接於 GPU 端傳輸至 DXGI SwapChain，完全不經過 CPU 記憶體複製。
* **多路獨立輸出 (Multi-Output Support)：** 支援同時配置高達 **4 組獨立顯示輸出 Channel** (`out1` ~ `out4`)。
* **彈性訊號路由 (Flexible Video Routing)：** 可為每個輸出通道獨立選擇 **Program (PGM 主畫面)** 或 **Preview (PVW 預覽畫面)**（極適合工作室模式/Studio Mode 監控需求）。
* **超低延遲引擎 (Ultra-Low Latency Engine)：** 採用獨立背景 Consumer 工作執行緒與混合 Spin-Wait 輪詢機制，達到亞毫秒級的畫面提交反應。
* **熱插拔與故障自動恢復 (Hot-Plug & Self-Recovery)：** 當遇到 DXGI Device Reset 或顯示拓樸變動時，提供自動連線監控與恢復機制。

---

## 💻 系統需求與前置條件

* **作業系統：** Windows 10 (Version 1903 / Build 18362 以上) 或 Windows 11 (64-bit)。
  * ⚠️ **版本限制注意：** 微軟的「特殊用途顯示器 (Specialized Display / 從桌面移除顯示器)」功能僅支援 **Windows Enterprise (企業版)**、**Windows Pro for Workstations (工作站專業版)** 以及 **Windows IoT Enterprise**。
  * ❌ **不支援版本：** 一般的 **Windows Pro (標準專業版)** 與 **Windows Home (家用版)** 均不支援此項微軟系統功能，無法在系統設定中將顯示器從桌面移除。
* **OBS Studio：** OBS Studio v28.0.0 或更高版本 (64-bit)。
* **硬體：** 支援 DirectX 11 的 NVIDIA、AMD 或 Intel 獨立/整合顯示卡。
* **開發與編譯工具：**
  * Visual Studio 2019 / 2022（需安裝 *使用 C++ 的桌面開發* 工作負載）。
  * CMake 3.16 或更新版本。
  * Windows 10/11 SDK。

---

## 🔨 如何編譯 (How to Build)

請依照以下步驟從原始碼編譯 `gpu-clean-output.dll`：

### 1. 複製專案庫 (Clone Repository)
```powershell
git clone https://github.com/your-username/GPU-Clean-Output.git
cd GPU-Clean-Output
```

### 2. 使用 CMake 進行配置
開啟 PowerShell 或 VS 2022 開發人員命令提示字元，建立並進入編譯目錄：
```powershell
mkdir build
cd build
```

若您有本地解開的 OBS Studio 相依套件，請透過 `OBS_SDK_DIR` 指定路徑：
```powershell
cmake -G "Visual Studio 17 2022" -A x64 -DOBS_SDK_DIR="C:/path/to/obs-studio-deps" ..
```
*（若系統環境變數已包含 `libobs`，直接執行 `cmake -G "Visual Studio 17 2022" -A x64 ..` 即可自動偵測。）*

### 3. 編譯外掛專案
使用 CMake 進行 Release 版本編譯：
```powershell
cmake --build . --config Release
```

編譯成功後，產出的 `.dll` 檔案將位於：
`build/Release/gpu-clean-output.dll`（或 `build/gpu-clean-output.dll`）。

---

## 📦 如何安裝 `.dll` 檔案

1. 若 OBS Studio 正在執行，請先將其完全關閉。
2. 找到編譯好的 `gpu-clean-output.dll` 檔案。
3. 將 `gpu-clean-output.dll` 複製到 OBS Studio 的外掛目錄中：
   * **標準安裝路徑 (64-bit)：**
     `C:\Program Files\obs-studio\obs-plugins\64bit\`
   * **便攜版安裝路徑 (Portable OBS Studio)：**
     `<OBS 安裝目錄>\obs-plugins\64bit\`
4. 啟動 OBS Studio。
5. 確認安裝：檢查 OBS 頂部選單欄是否出現 **工具 (Tools)** -> **`GPU Clean Output (Specialized Display)`**。

---

## 🖥️ 如何觸發螢幕獨佔功能 (Specialized Display & Direct Scanout)

要觸發硬體獨佔直輸出功能，需先於 Windows 系統中將目標顯示器設定為「特殊用途顯示器」，接著於 OBS Studio 中啟動輸出。

### 步驟 1：設定 Windows「特殊用途顯示器 (Specialized Display)」模式

將您的廣播/監控實體螢幕脫離 Windows DWM 桌面管理：

1. 將目標 HDMI 或 DisplayPort 顯示器連接至電腦。
2. 開啟 **Windows 設定** (`Win + I`)。
3. 進入 **系統** -> **顯示器** -> **進階顯示設定**（Windows 11 為 **進階顯示**）。
4. 於頂部的顯示器下拉選單中，選取您的目標顯示器。
5. 找到 **「從桌面移除顯示器 (Remove display from desktop)」** 選項（或 **特殊用途顯示器** 開關），並將其切換為 **開啟 (ON)**。
6. Windows 將立即將該顯示器自桌面視窗管理中移除（該顯示器將不再顯示 Windows 桌面背景或滑鼠游標）。

> ⚠️ **重要版本提示：** 若在進階顯示設定中**找不到「從桌面移除顯示器」選項或開關**，請確認您的 Windows 版本是否為 **Enterprise (企業版)、Pro for Workstations (工作站專業版) 或 IoT Enterprise**。標準 **Windows Pro (專業版)** 與 **Windows Home (家用版)** 系統中均未提供此微軟功能。
>
> ℹ️ **注意：** 在 `GPU-Clean-Output` 尚未接管並開始輸出畫面之前，該顯示器會暫時呈現黑畫面或進入待機狀態，此為正常現象。

---

### 步驟 2：於 OBS Studio 中啟動硬體直輸出

1. 開啟 **OBS Studio**。
2. 點選頂部選單欄的 **工具 (Tools)** -> **`GPU Clean Output (Specialized Display)`**。
3. 於外掛設定視窗中進行設定：
   * 勾選 **Enable Direct Output (啟動硬體直輸出)**。
   * **Target Specialized Display 1**：於下拉選單中選取您的目標顯示器（可選擇 `Auto-Detect First Specialized Display` 或指定該顯示器的硬體名稱）。
   * **Source Output 1**：選擇畫面訊號源 **Program (PGM 主畫面)** 或 **Preview (PVW 預覽畫面)**。
   * *(可選)* 若您連接了多台特殊用途顯示器，可依需求配置 Output Slot 2 至 Slot 4。
4. 點選 **確定 (OK)**。

目標實體顯示器將會瞬間點亮，並開始接收由 GPU 直出、極低延遲的純淨畫面！

---

## 🛠️ 技術架構摘要 (Architecture Summary)

```
[ OBS 渲染引擎 ] ---> (GPU 零拷貝共享貼圖) ---> [ Direct3D 11 渲染器 ]
                                                           |
                                                DXGI SwapChain Present
                                                           v
[ WinRT DisplayTarget ] <--- (DisplayCore API) <--- [ 獨佔實體顯示器 ]
```

* **API 層：** 使用 WinRT `Windows.Devices.Display.Core::DisplayManager` 進行硬體 Target 列舉、鎖定與 DisplayState 狀態管理。
* **渲染引擎：** 建立綁定至該顯示器實體 GPU Adapter (LUID) 的獨立 D3D11 Device。
* **渲染管道：** 搭配零拷貝貼圖共享與亞毫秒級佇列刷洗 (Queue Flush)。

---

## 📄 授權條款 (License)

本專案採用 GPL-2.0 授權條款 - 詳情請參閱 [LICENSE](LICENSE) 檔案。
