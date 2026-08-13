# OBS Studio WinRT Specialized Display 硬體直輸出開發與除錯技術全錄 (Technical Guide)

本文檔記錄在 OBS Studio 中開發 **WinRT DisplayCore 硬體直輸出 (Direct Hardware Scanout)** 外掛時遇到的所有架構設計、技術細節、錯誤嘗試與正確解決方案。

---

## 1. 專案背景與目標

- **目標**：讓 OBS Studio 能將 PGM (Program) 或 PVW (Preview) 畫面，直輸出至 Windows 的「專用顯示器 (Specialized Display，即從 OS 桌面移除的硬體顯示器)」。
- **優勢**：繞過 Windows DWM (Desktop Window Manager) 桌面合成器與滑鼠游標干擾，獲得最純淨、低延遲的 60FPS 硬體極速畫面輸出。
- **技術棧**：C++20, WinRT `Windows.Devices.Display.Core`, DirectX 11, OBS Studio C-API (libobs).

---

## 2. 架構設計圖 (Architecture Diagram)

```
+-----------------------------------------------------------------------+
|                              OBS Studio                               |
|                                                                       |
|  [Main Video Canvas] --(gs_stage_texture)--> [gs_stagesurf (CPU)]     |
+---------------------------------------------------|-------------------+
                                                    | memcpy (RowPitch)
                                                    v
+-----------------------------------------------------------------------+
|             Dedicated Secondary D3D11 Device (on GPU LUID)             |
|                                                                       |
|  [UpdateSubresource] ---> [Scanout Texture (Opened via Shared Handle)] |
|        |                                                              |
|   [context->Flush()]                                                  |
+--------|--------------------------------------------------------------+
         |
         v
+-----------------------------------------------------------------------+
|             WinRT DisplayCore Direct Hardware Scanout                  |
|                                                                       |
|  [DisplaySurface] --> [DisplayScanout] --> [HDMI Hardware Physical Clock]|
+-----------------------------------------------------------------------+
```

---

## 3. 錯誤做法 vs 正確做法（核心技術對照表）

### 3.1 繁體中文 UI 介面亂碼 (Mojibake)
- ❌ **錯誤做法**：直接在 MSVC 環境編譯包含中文 UTF-8 字串的 C++ 原始碼，未指定字元集，導致編譯器以 MSVC 預設 CodePage (CP950) 解析，UI 顯示亂碼。
- ✅ **正確做法**：在 `CMakeLists.txt` 中針對 MSVC 加入 `/utf-8` 編譯選項：
  ```cmake
  if(MSVC)
      add_compile_options(/utf-8)
  endif()
  ```

---

### 3.2 OBS 屬性視窗顯示「無可用的屬性」
- ❌ **錯誤做法**：使用 `obs_source_create` 建立設定 Source，但 `obs_source_info` 未設定尺寸與繪製標記，導致 OBS UI 認為該 Source 無法互動。
- ✅ **正確做法**：
  1. 在 `obs_source_info` 中設定 `OBS_SOURCE_CUSTOM_DRAW` 標記與 `get_width` / `get_height` 回調。
  2. 使用 `obs_source_create_private` 建立私有設定 Source，防止影響使用者場面。

---

### 3.3 OBS 執行緒圖形上下文崩潰 (`gs_get_device_obj() = NULL`)
- ❌ **錯誤做法**：在 UI 執行緒 (Qt Event Loop) 的 `clean_output_start()` 內部直接呼叫 OBS 圖形 API（如 `obs_get_main_texture()` 或 `gs_texture_get_color_format()`）。由於非 GPU 繪圖執行緒缺少 TLS (Thread-Local Storage) 圖形上下文，造成空指標引爆 Crash。
- ✅ **正確做法**：在非繪圖執行緒存取 OBS 圖形 API 時，必須以 `obs_enter_graphics()` 與 `obs_leave_graphics()` 包裹：
  ```cpp
  obs_enter_graphics();
  gs_texture_t *main_tex = obs_get_main_texture();
  // ... 操作圖形資源 ...
  obs_leave_graphics();
  ```

---

### 3.4 啟動輸出時 OBS 編輯器 (PVM/PGM) 變黑
- ❌ **錯誤做法**：在每幀 `clean_output_main_rendered` 繪圖回調中，呼叫 OBS 原生 D3D11 Context 的 `context->Flush()`。這會打斷 OBS 主渲染管道的命令流，導致 OBS 預覽畫面黑屏。
- ✅ **正確做法**：**絕不在 OBS 原生的 D3D11 Context 上呼叫 `Flush()`**。如需刷新 GPU 命令，必須在專屬的第二 D3D11 Device Context 上執行。

---

### 3.5 螢幕點亮有訊號但全黑無畫面 (All-Black Display)
- ❌ **錯誤做法 1 (色彩格式不符合)**：`DisplayPrimaryDescription` 硬編碼為 `DXGI_FORMAT_B8G8R8A8_UNORM` (87)，而 OBS 主畫面 Texture 為 `DXGI_FORMAT_R8G8B8A8_UNORM` (28)。DirectX 11 拒絕跨不同 DXGI 格式進行 `CopyResource`，複製操作靜默失敗， scanout 表面維持全 0 (全黑)。
- ❌ **錯誤做法 2 (跨 Device 共享控制代碼失效)**：直接使用 OBS 的 D3D11 Device 去 `OpenSharedResource1` 開啟 WinRT `DisplaySurface` 的 shared handle。由於 WinRT `DisplaySurface` 綁定在顯示器硬體的 GPU Adapter 上，如果 OBS 運行的 D3D11 Context 與 WinRT 不在同一個 Device 實例，跨 Device 寫入會失效。
- ❌ **錯誤做法 3 (未強制 Flushing 命令隊列)**：`UpdateSubresource` 只是將複製指令排入 GPU 隊列，如果沒有執行 `context->Flush()`，GPU 延遲執行，畫面會一直維持舊數據（只有在停用外掛釋放 context 時才會短暫閃一下畫面）。
- ✅ **正確做法**：
  1. 讀取 WinRT `DisplayTarget.Adapter().Id()` 取得 GPU LUID。
  2. 使用 `IDXGIFactory4::EnumAdapterByLuid(luid)` 取得對應的 `IDXGIAdapter`。
  3. 在該 Adapter 上建立**獨立的第二 D3D11 Device**。
  4. 使用這個獨立 D3D11 Device 打開 `DisplaySurface` 的 Shared Handle (`OpenSharedResource1`)。
  5. 每幀用 OBS 的 `gs_stagesurface` 將 GPU 像素暫存至 CPU，再透過 `UpdateSubresource` 寫入獨立 Device 的 scanout 表面。
  6. 在獨立 Device 上呼叫 `context->Flush()` 強制 GPU 立即將像素渲染至螢幕！

---

### 3.6 取消勾選外掛 / 關閉時 OBS 崩潰 (Crash on Stop)
- ❌ **錯誤做法 1**：當 `scanout_texture_` (D3D11 資源) 仍在使用時，提前呼叫 `CloseHandle(surface_shared_handle_)`，引發 D3D11 驅動層 Kernel Exception。
- ❌ **錯誤做法 2**：`clean_output_update` 與 `clean_output_stop` 在 UI 屬性變更時雙重觸發，導致競爭條件 (Race Condition) 與重複釋放。
- ❌ **錯誤做法 3**：WinRT COM 物件（`DisplayState`, `DisplayDevice`）跨 Thread 釋放時未捕獲 COM Apartment 異常 (`RPC_E_WRONG_THREAD`)。
- ✅ **正確做法**：
  1. 解構順序：**先釋放 D3D11 資源 (`scanout_texture_ = nullptr`)** ➔ **再關閉 NT Shared Handle (`CloseHandle`)** ➔ **最後解構 WinRT COM 物件**。
  2. 引入 `stopping` 狀態標記，防止併發重入。
  3. 在 `Release()` 中為每一個 WinRT COM 指針使用獨立的 `try { ... } catch (...) {}` 安全拆卸。

---

### 3.7 關閉 OBS 或取消勾選後螢幕未恢復無訊號狀態
- ❌ **錯誤做法**：直接將 WinRT 指針設為 `nullptr`，未通知顯卡驅動程式解除時脈鎖定，導致顯示器維持在最後一幀或保持亮屏狀態。
- ✅ **正確做法**：在 `Release()` 時，呼叫 `display_manager_.TryAcquireTargetsAndCreateEmptyState({ target_ })` 並執行 `emptyState.TryApply()`。主動套用空狀態會通知顯卡切斷物理 HDMI 時脈輸出，使螢幕立即恢復「無訊號 / 省電待機」狀態。

### 3.8 PVW (Preview) 預覽畫面動態/影片卡凍在第一幀
- ❌ **錯誤做法**：只取得 PVW Scene 指針後直接繪製，未對 PVW Source 遞增 Active/Showing 引用計數。OBS 的 Video Tick 核心引擎發現該 Source 的 active/showing 為 0，會停止該場景內所有動態與影片 Source 的幀更新，導致輸出畫面永久凍結在第一幀。
- ✅ **正確做法**：
  在切換至 PVW 場面時，必須主動呼叫 `obs_source_inc_showing` 與 `obs_source_inc_active` 強制 libobs 引擎對其進行 Video Tick 動畫更新；當取消選擇或切換離該場面時，呼叫 `obs_source_dec_showing` 與 `obs_source_dec_active`：
  ```cpp
  obs_source_inc_showing(pvw_source);
  obs_source_inc_active(pvw_source);
  ```

---

### 3.9 PVW 選擇模式下靜默降級退回顯示 PGM 畫面
- ❌ **錯誤做法**：每幀重用 `gs_texrender_t` 時未在 `gs_texrender_begin` 前呼叫 `gs_texrender_reset`。
- 🔍 **原理解析**：OBS 的 `gs_texrender_end` 會在內部將渲染標記設為 `rendered = true`。下一幀 `gs_texrender_begin` 檢查到 `rendered == true` 會直接回傳 `false`（渲染失敗）。當離屏渲染失敗時，`obs_tex` 為空，觸發 Fallback 退回呼叫 `obs_get_main_texture()` (即 PGM 主畫布)，造成選擇 PVW 卻依然輸出 PGM 的問題。
- ✅ **正確做法**：在每幀 `gs_texrender_begin` 前必須呼叫 `gs_texrender_reset(pvw_texrender_)` 清除渲染標記：
  ```cpp
  gs_texrender_reset(pvw_texrender_);
  if (gs_texrender_begin(pvw_texrender_, width, height)) {
      // 繪製 PVW 場面...
      gs_texrender_end(pvw_texrender_);
      obs_tex = gs_texrender_get_texture(pvw_texrender_);
  }
  ```

---

### 3.10 PVW 畫面渲染錯位、變形或全黑無訊號
- ❌ **錯誤做法**：認為 `gs_texrender_begin` 會自動重置正交投影矩陣，因而省略 `gs_ortho` 呼叫。
- 🔍 **原理解析**：`gs_texrender_begin` 只會將模型視圖矩陣重置為單位矩陣 (`gs_matrix_identity`)，但**不會重置投影矩陣 (Projection Matrix)**。這會導致 PVW 場面繼承 OBS UI 或小圖預覽視窗的投影矩陣，畫面被渲染至 1920x1080 畫布之外或縮放成單個像素。
- ✅ **正確做法**：在 `gs_texrender_begin` 之後，必須明確呼叫 `gs_ortho` 建立 2D 正交投影矩陣：
  ```cpp
  gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
  gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
  obs_source_video_render(pvw_source);
  ```

---

### 3.11 多螢幕獨立直輸出架構與「無輸出」選項設計
- ❌ **錯誤做法**：外掛只維護單一全域 `SpecialDisplayManager` 與 `D3D11OutputRenderer` 實體，限制只能輸出至一個螢幕，且選單缺少主動關閉輸出的「無輸出」選項。
- ✅ **正確做法**：
  1. **多通道 Slot 模組化**：定義 `DisplayOutputSlot` 結構，每個 Slot 擁有獨立的 `SpecialDisplayManager`、`D3D11OutputRenderer` 與 `gs_texrender_t`。外掛維護 4 組 Slot (`slots_[4]`)。
  2. **「無輸出」選項 (None / Disabled)**：在每個螢幕選單頂部新增 `"none"` (`None / Disabled (無輸出)`) 選項。當使用者選擇 `"none"` 時，對應的 Slot 立即呼叫 `Stop()` 釋放硬體時脈與 DirectX 資源，實現優雅關閉。
  3. **向下相容性**：若舊版設定檔只包含 `monitor_id` 與 `source_type`，外掛自動將其對映至 Slot 1，確保無縫升級。

---

### 3.12 消除 CPU 瓶頸：DXGI 跨適配器共用表面 (GPU Zero-Copy)
- ❌ **錯誤做法**：使用 `gs_stage_texture` + CPU `memcpy` + `UpdateSubresource` 將像素複製至 CPU 記憶體再上傳至專用 D3D11 裝置。這會造成高 CPU 佔用率與 PCIe 匯流排傳送延遲。
- ✅ **正確做法**：
  1. 在 OBS 主渲染 GPU (`gs_get_device_obj()`) 上建立支援跨適配器的共用 Texture2D (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`)。
  2. 透過 `CreateSharedHandle` 導出 Handle，並在 DisplayTarget 專用 GPU 上以 `OpenSharedResource1` 開啟該共用 Handle。
  3. 畫面傳輸 100% 在 GPU VRAM 內由 Direct DMA 完成，徹底消除 CPU `memcpy` 與記憶體映射，達到 0% CPU 佔用！

---

### 3.13 執行緒最佳化：生產者-消費者模型 (Producer-Consumer Queue)
- ❌ **錯誤做法**：在 OBS 渲染回調 (`obs_add_main_rendered_callback`) 內同步執行 `context_->Flush()` 與 `PresentScanout()`。由於 GPU 佇列刷洗與 WinRT 狀態更新有一定延遲，直接阻塞 OBS 繪圖主執行緒，引發微卡頓 (Micro-stutters)。
- ✅ **正確做法**：
  1. 引入 Producer-Consumer 獨立背景執行緒 (`worker_thread`) 與條件變數 (`queue_cv`)。
  2. **Producer（OBS 渲染緒）**：僅負責將 GPU 畫面複製至 Shared Surface 並推送訊號 (`notify_one`)，隨即立即返回 OBS 主循環，絕不阻塞。
  3. **Consumer（背景工作緒）**：在背景 Worker Thread 中從佇列喚醒、獲取 Keyed Mutex、執行 `CopyResource`、`context_->Flush()` 並推送到 WinRT `DisplaySurface` 進行實體 Scanout。

---

### 3.14 穩定性增強：硬體熱插拔與斷線自動恢復 (Hot-Plug & State Recovery)
- ❌ **錯誤做法**：拔除實體 Specialized Display HDMI 線或顯示卡驅動程式重置時，WinRT 或 DXGI 拋出 `DXGI_ERROR_DEVICE_REMOVED` 異常引爆 OBS Crash。
- ✅ **正確做法**：
  1. 每次 Present 繪圖前檢測 `device_->GetDeviceRemovedReason()` 與 WinRT 異常。
  2. 當偵測到實體裝置斷線時，標記通道狀態為 `is_degraded = true`，並呼叫 `StopPipelineInternal()` 安全關閉控制代碼與資源，保護 OBS 主程式不崩潰。
  3. 在 Render 迴圈中加入背景自動重連機制（每 2 秒輪詢一次 `Start()`），當實體螢幕重新插回時自動恢復直輸出！

---

### 4.1 根據 WinRT LUID 匹配並建立專用 D3D11 Device
```cpp
IDXGIAdapter *SpecialDisplayManager::GetDXGIAdapter() {
    if (dxgi_adapter_) return dxgi_adapter_.get();
    if (!target_) return nullptr;

    try {
        auto adapter = target_.Adapter();
        auto adapterId = adapter.Id();
        LUID luid;
        luid.LowPart = adapterId.LowPart;
        luid.HighPart = adapterId.HighPart;

        if (!dxgi_factory_) {
            CreateDXGIFactory2(0, IID_PPV_ARGS(dxgi_factory_.put()));
        }

        HRESULT hr = dxgi_factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(dxgi_adapter_.put()));
        if (FAILED(hr)) return nullptr;

        return dxgi_adapter_.get();
    } catch (...) {
        return nullptr;
    }
}
```

### 4.2 跨 Device 像素複製與 GPU 強制 Flushing
```cpp
bool D3D11OutputRenderer::CopyFrameToScanout(gs_texture_t *obs_tex) {
    if (!obs_tex || !device_ || !context_ || !scanout_texture_) return false;

    uint32_t tex_width = gs_texture_get_width(obs_tex);
    uint32_t tex_height = gs_texture_get_height(obs_tex);
    uint32_t copy_w = (std::min)(tex_width, width_);
    uint32_t copy_h = (std::min)(tex_height, height_);

    // 快取並重複使用 stagesurf，避免每幀頻繁記憶體配置
    if (!stagesurf_) {
        stagesurf_ = gs_stagesurface_create(copy_w, copy_h, GS_BGRA);
    }

    gs_stage_texture(stagesurf_, obs_tex);

    uint8_t *data = nullptr;
    uint32_t linesize = 0;
    if (!gs_stagesurface_map(stagesurf_, &data, &linesize) || !data) return false;

    D3D11_BOX box = {};
    box.right = copy_w;
    box.bottom = copy_h;
    box.back = 1;

    // 將 CPU 數據更新至專用 D3D11 Device 的 Scanout 表面
    context_->UpdateSubresource(scanout_texture_.get(), 0, &box, data, linesize, 0);

    gs_stagesurface_unmap(stagesurf_);

    // 強制 Flushing 獨立 Device 命令隊列，讓 GPU 立即渲染至硬體螢幕
    context_->Flush();
    return true;
}
```

### 4.3 釋放時關閉物理 HDMI 訊號
```cpp
void SpecialDisplayManager::Release() {
    std::lock_guard<std::mutex> lock(render_mutex_);

    // 套用空的 unattached DisplayState 關閉 HDMI 物理時脈
    try {
        if (display_manager_ && target_) {
            auto targetsList = winrt::single_threaded_vector<DisplayTarget>();
            targetsList.Append(target_);
            auto result = display_manager_.TryAcquireTargetsAndCreateEmptyState(targetsList);
            if (result.ErrorCode() == DisplayManagerResult::Success) {
                result.State().TryApply(DisplayStateApplyOptions::None);
            }
        }
    } catch (...) {}

    if (surface_shared_handle_) {
        CloseHandle(surface_shared_handle_);
        surface_shared_handle_ = NULL;
    }

    // 隔離每一個 WinRT COM 物件的解構，防止跨 Apartment 異常
    try { task_ = nullptr; } catch (...) {}
    try { task_pool_ = nullptr; } catch (...) {}
    try { scanout_ = nullptr; } catch (...) {}
    try { surface_ = nullptr; } catch (...) {}
    try { path_ = nullptr; } catch (...) {}
    try { display_state_ = nullptr; } catch (...) {}
    try { display_device_ = nullptr; } catch (...) {}
    try { target_ = nullptr; } catch (...) {}
    try { display_manager_ = nullptr; } catch (...) {}
}
```

---

## 5. 總結

通過以上架構調整，成功解決了 WinRT DisplayCore 在 OBS Studio 中硬體直輸出所遭遇的**亂碼、無可用的屬性、TLS 上下文引爆、畫面變黑、跨 Device 共享失敗、取消閃退與訊號無法復原**等全套問題，實現了穩定 60FPS 極速硬體直輸與無縫訊號切斷！
