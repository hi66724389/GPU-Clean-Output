# GPU Clean Output (GPU-Clean-Output) - Project Plan

## 1. Project Overview
本專案旨在開發一個原生的 OBS Studio C/C++ 輸出外掛 (Output Plugin) —— **GPU-Clean-Output**。
目標是攔截 OBS 的最終渲染畫面，完全繞過 Windows 桌面視窗管理員 (DWM)，將影像以極低延遲 (Zero-Copy) 直接輸出至被 Windows 設定為「從桌面移除 (Specialized Display)」的實體 HDMI 螢幕。

## 2. Architecture & Foundation
*   **Base Framework:** 基於 OBS Studio 原生 Plugin 架構進行開發與 CMake 構建。
*   **Language:** C++20，搭配 C++/WinRT 處理 Windows 現代 API。
*   **OS Support:** Windows 10/11 (64-bit) —— **需為 Windows Enterprise (企業版)、Windows Pro for Workstations (工作站專業版) 或 Windows IoT Enterprise**；標準 Windows Pro (專業版) 與 Windows Home (家用版) 均不支援微軟「特殊用途顯示器 (Specialized Display / 從桌面移除顯示器)」系統功能。

## 3. Core Modules & Implementation Details
### 3.1 Plugin Scaffolding
*   使用 `obs_output_info` 註冊 Output 類型 (id: `d3d11_special_display_output`) 與 Source 類型 (`d3d11_special_display_source`)。
*   實作必備的生命週期回呼函式：`.create`, `.destroy`, `.start`, `.stop`。

### 3.2 DisplayCore API Integration
*   使用 WinRT API `Windows.Devices.Display.Core::DisplayManager`。
*   掃描系統中的 `DisplayTarget`，篩選出 `UsageKind` 為 `SpecialPurpose` 的實體連接輸出目標。
*   建立 `DisplayState` 並呼叫 `TryApply()` 鎖定並接管該顯示器，取得獨佔權。

### 3.3 Direct3D 11 Exclusive SwapChain / Scanout
*   使用 `D3D11CreateDevice` 在對應的硬體配接器 (Adapter LUID) 上建立 D3D11 Device。
*   建立直接綁定該 `DisplayTarget` 的獨佔 Scanout，確保廣播級的超低延遲。

### 3.4 Zero-Copy Rendering (效能紅線約束)
*   **【嚴格約束】**：絕對禁止將 OBS 畫面 (Texture) 拷貝回系統記憶體 (RAM)，也絕對禁止撰寫任何 CPU For-Loop 來處理像素轉換 (例如 RGBA 轉 UYVY)。
*   **實作路徑**：
    1. 使用 `obs_add_main_rendered_callback` 攔截 OBS Render Target。
    2. 建立 Triple-Buffer GPU Shared NT Handle 資源池。
    3. 透過 GPU 直接將該 Texture `Present` 到 Scanout Surface 上，達成 100% GPU Zero-Copy 傳輸。

### 3.5 Plugin Properties & UI (使用者介面與設定)
*   **介面建構 (`get_properties`)**：
    *   使用 `obs_properties_create()` 建立屬性面板。
    *   支援高達 4 組獨立顯示通道 (`out1` ~ `out4`)。
    *   **輸出螢幕選擇 (Monitor Selection)**：下拉選單動態列舉所有 `Specialized` 顯示器。
    *   **輸出訊號源 (Source Selection)**：支援獨立切換 `Program (PGM)` 與 `Preview (PVW)`。

## 4. Acceptance Criteria
*   程式碼能透過 CMake 成功編譯為 `gpu-clean-output.dll` 並載入 OBS。
*   啟動輸出後，指定的特殊用途螢幕能顯示 OBS 的 Clean Feed。
*   CPU 使用率維持極低，完全由 GPU 零拷貝處理。