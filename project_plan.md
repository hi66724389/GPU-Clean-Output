# OBS Hardware Direct Output Plugin - Project Plan

## 1. Project Overview
本專案旨在開發一個原生的 OBS Studio C/C++ 輸出外掛 (Output Plugin)。
目標是攔截 OBS 的最終渲染畫面，完全繞過 Windows 桌面視窗管理員 (DWM)，將影像以極低延遲 (Zero-Copy) 直接輸出至被 Windows 設定為「從桌面移除 (Specialized Display)」的實體 HDMI 螢幕。

## 2. Architecture & Foundation
*   **Base Framework:** 必須嚴格基於官方開源模板 `obsproject/obs-plugintemplate` 進行開發與 CMake 構建。
*   **Language:** C++17 或以上，搭配 C++/WinRT 處理 Windows 現代 API。
*   **Graphics API:** Direct3D 11 (D3D11) 與 DXGI。
*   **OS Support:** Windows 10/11。

## 3. Core Modules & Implementation Details
### 3.1 Plugin Scaffolding (基於 obs-plugintemplate)
*   使用 `obs_output_info` 註冊一個新的 Output 類型 (id: `d3d11_special_display_output`)。
*   實作必備的生命週期回呼函式：`.create`, `.destroy`, `.start`, `.stop`。

### 3.2 DisplayCore API Integration
*   使用 WinRT API `Windows.Devices.Display.Core::DisplayManager`。
*   掃描系統中的 `DisplayTarget`，篩選出 `UsageKind` 為 `Specialized` 的實體連接輸出目標。
*   建立 `DisplayState` 並呼叫 `TryApply()` 鎖定並接管該顯示器，取得獨佔權。

### 3.3 Direct3D 11 Exclusive SwapChain
*   使用 `D3D11CreateDevice` 在對應的硬體配接器 (Adapter) 上建立 D3D11 Device。
*   建立直接綁定該 `DisplayTarget` 的獨佔 SwapChain，設定為 Flip Sequential 模式以確保廣播級的低延遲。

### 3.4 Zero-Copy Rendering (效能紅線約束)
*   **【嚴格約束】**：絕對禁止將 OBS 畫面 (Texture) 拷貝回系統記憶體 (RAM)，也絕對禁止撰寫任何 CPU For-Loop 來處理像素轉換 (例如 RGBA 轉 UYVY)。這會導致 CPU 資源瞬間耗盡。
*   **實作路徑**：
    1. 使用 `obs_add_main_render_callback` 攔截 OBS Render Target。
    2. 透過 `gs_texture_get_shared_handle` 提取底層的 D3D11 Shared Texture Handle。
    3. 在我們的 D3D11 Device 中使用 `OpenSharedResource` 開啟該 Handle。
    4. 透過 GPU 直接將該 Texture `Present` 到我們的 SwapChain 上，達成 100% GPU Zero-Copy 傳輸。

### 3.5 Plugin Properties & UI (使用者介面與設定)
為了讓使用者能動態選擇輸出目標與畫面來源，必須實作 OBS 的屬性介面。
*   **介面建構 (`get_properties`)**：
    *   使用 `obs_properties_create()` 建立屬性面板。
    *   **輸出螢幕選擇 (Monitor Selection)**：使用 `obs_properties_add_list` 建立下拉式選單。呼叫 DisplayCore API 重新掃描系統中所有 `Specialized` 的顯示器，並將顯示器的名稱或 ID 填入選單中。
    *   **輸出訊號源 (Source Selection)**：使用 `obs_properties_add_list` 建立第二個下拉式選單，提供兩個選項：`Program (PGM)` 與 `Preview (PVW)`。
*   **設定套用 (`update` / `get_defaults`)**：
    *   透過 `obs_data_t` 讀取使用者的選擇。
    *   當使用者切換螢幕時，必須安全地釋放舊的 SwapChain 並在新的 DisplayTarget 上重新建立。
*   **PVW/PGM 路由邏輯 (Routing Logic)**：
    *   **PGM (預設)**：維持攔截 Main Render Callback 取得最終輸出的 Shared Texture。
    *   **PVW (預覽)**：當處於工作室模式 (Studio Mode) 時，必須透過 OBS API 取得 Preview 場景的 Texture Handle，而非 Main Mix。要求 AI 實作動態切換 Render Target 的邏輯。

## 4. Acceptance Criteria
*   程式碼能透過 CMake 成功編譯為 `.dll` 並載入 OBS。
*   啟動輸出後，指定的特殊用途螢幕能顯示 OBS 的 Clean Feed。
*   CPU 使用率必須極低，不可因為啟用此輸出而產生異常波動。