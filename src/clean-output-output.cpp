#include "clean-output-output.hpp"
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <graphics/vec4.h>

// Global singleton instance for clean output management
CleanOutputContext g_clean_output;

static void clean_output_main_rendered(void *param) {
    UNUSED_PARAMETER(param);
    g_clean_output.RenderFrame();
}

DisplayOutputSlot::DisplayOutputSlot()
    : display_manager(std::make_unique<SpecialDisplayManager>()),
      renderer(std::make_unique<D3D11OutputRenderer>()) {}

DisplayOutputSlot::~DisplayOutputSlot() {
    Stop();
}

void DisplayOutputSlot::UpdateShowingSource(obs_source_t *new_source) {
    if (pvw_showing_source == new_source) return;

    if (pvw_showing_source) {
        obs_source_dec_active(pvw_showing_source);
        obs_source_dec_showing(pvw_showing_source);
        obs_source_release(pvw_showing_source);
        pvw_showing_source = nullptr;
    }

    if (new_source) {
        obs_source_inc_showing(new_source);
        obs_source_inc_active(new_source);
        pvw_showing_source = obs_source_get_ref(new_source);
        if (new_source) {
            const char *name = obs_source_get_name(new_source);
            blog(LOG_INFO, "[GPU-Clean-Output] Slot %d: Activated PVW source: %s", index + 1, name ? name : "(null)");
        }
    }
}

bool DisplayOutputSlot::Start() {
    if (active || target_id == "none") return false;

    blog(LOG_INFO, "[GPU-Clean-Output] Slot %d starting for target: %s...", index + 1, target_id.c_str());

    if (!display_manager->Initialize(target_id)) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Slot %d: Failed to initialize DisplayManager for target: %s", index + 1, target_id.c_str());
        return false;
    }

    if (!display_manager->TryApplyAndLock()) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Slot %d: Failed to lock specialized display", index + 1);
        return false;
    }

    uint32_t width = display_manager->GetWidth();
    uint32_t height = display_manager->GetHeight();

    IDXGIAdapter *adapter = display_manager->GetDXGIAdapter();
    if (!adapter) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Slot %d: Failed to get DXGI adapter", index + 1);
        display_manager->Release();
        return false;
    }

    if (!renderer->InitializeOnAdapter(adapter, width, height)) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Slot %d: Failed to initialize D3D11 renderer", index + 1);
        display_manager->Release();
        return false;
    }

    HANDLE surface_handle = display_manager->GetSurfaceSharedHandle();
    if (!surface_handle || !renderer->BindScanoutTarget(surface_handle)) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Slot %d: Failed to bind scanout target", index + 1);
        renderer->Cleanup();
        display_manager->Release();
        return false;
    }

    // Initialize GPU Zero-Copy Shared Surface
    if (!renderer->InitializeGPUSharedTexture()) {
        blog(LOG_WARNING, "[GPU-Clean-Output] Slot %d: GPU Zero-Copy texture init warning, using staging fallback.", index + 1);
    }

    // Launch Consumer worker thread
    thread_running = true;
    pending_frames = 0;
    worker_thread = std::thread(&DisplayOutputSlot::WorkerThreadLoop, this);

    active = true;
    is_degraded = false;
    blog(LOG_INFO, "[GPU-Clean-Output] Slot %d started successfully! (%ux%u)", index + 1, width, height);
    return true;
}

void DisplayOutputSlot::StopPipelineInternal() {
    UpdateShowingSource(nullptr);

    obs_enter_graphics();
    if (pvw_texrender) {
        gs_texrender_destroy(pvw_texrender);
        pvw_texrender = nullptr;
    }
    if (renderer) {
        renderer->DestroyStagesurf();
    }
    obs_leave_graphics();

    if (renderer) {
        renderer->Cleanup();
    }

    if (display_manager) {
        display_manager->Release();
    }
}

void DisplayOutputSlot::Stop() {
    active = false;
    is_degraded = false;

    if (thread_running) {
        thread_running = false;
        queue_cv.notify_all();
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }

    StopPipelineInternal();
    blog(LOG_INFO, "[GPU-Clean-Output] Slot %d stopped.", index + 1);
}

void DisplayOutputSlot::WorkerThreadLoop() {
    blog(LOG_INFO, "[GPU-Clean-Output] Slot %d ultra-low latency consumer thread started.", index + 1);

    while (thread_running) {
        // Hybrid Spin-Wait: Spin for up to 500 iterations for sub-millisecond wakeup response
        bool frame_found = false;
        for (int spin = 0; spin < 500; ++spin) {
            if (pending_frames.load(std::memory_order_relaxed) > 0) {
                frame_found = true;
                break;
            }
            std::this_thread::yield();
        }

        if (!frame_found && thread_running) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait_for(lock, std::chrono::milliseconds(5), [this] {
                return !thread_running || pending_frames.load(std::memory_order_relaxed) > 0;
            });
        }

        if (!thread_running) break;

        int count = pending_frames.exchange(0, std::memory_order_acq_rel);
        if (count > 0 && renderer && display_manager) {
            if (renderer->IsDeviceRemoved()) {
                blog(LOG_WARNING, "[GPU-Clean-Output] Slot %d: DXGI device removal detected in background thread!", index + 1);
                is_degraded = true;
                break;
            }

            if (renderer->ConsumerPresentFrameGPU()) {
                display_manager->PresentScanout();
            }
        }
    }

    blog(LOG_INFO, "[GPU-Clean-Output] Slot %d consumer thread exiting.", index + 1);
}

void DisplayOutputSlot::Render(obs_source_t *pvw_source) {
    // Check for Hot-Plug / Fault Degraded state and attempt auto-recovery
    if (is_degraded) {
        uint64_t now = os_gettime_ns();
        if (now - last_reconnect_time > 2000000000ULL) { // Retry every 2 seconds
            last_reconnect_time = now;
            blog(LOG_INFO, "[GPU-Clean-Output] Slot %d: Attempting hot-plug auto-reconnection...", index + 1);

            Stop();
            if (Start()) {
                blog(LOG_INFO, "[GPU-Clean-Output] Slot %d: Hot-plug auto-reconnection successful!", index + 1);
                return;
            }
        }
        return;
    }

    if (!active || !renderer || !display_manager) return;

    gs_texture_t *obs_tex = nullptr;

    if (use_preview) {
        UpdateShowingSource(pvw_source);

        if (pvw_source) {
            uint32_t width = obs_source_get_base_width(pvw_source);
            uint32_t height = obs_source_get_base_height(pvw_source);

            if (width == 0 || height == 0) {
                struct obs_video_info ovi;
                if (obs_get_video_info(&ovi)) {
                    width = ovi.base_width;
                    height = ovi.base_height;
                }
            }

            if (width > 0 && height > 0) {
                if (!pvw_texrender) {
                    pvw_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
                }
                if (pvw_texrender) {
                    gs_texrender_reset(pvw_texrender);
                    if (gs_texrender_begin(pvw_texrender, width, height)) {
                        struct vec4 background;
                        vec4_zero(&background);
                        gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);

                        gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);

                        gs_blend_state_push();
                        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

                        obs_source_video_render(pvw_source);

                        gs_blend_state_pop();

                        gs_texrender_end(pvw_texrender);
                        obs_tex = gs_texrender_get_texture(pvw_texrender);
                    }
                }
            }
        }
    } else {
        UpdateShowingSource(nullptr);
    }

    if (!obs_tex) {
        obs_tex = obs_get_main_texture();
    }

    if (obs_tex) {
        // Producer: Copy frame on GPU (Zero-Copy) and notify consumer thread
        if (renderer->ProducerCopyFrameGPU(obs_tex)) {
            pending_frames = 1;
            queue_cv.notify_one();
        } else {
            // Fallback for non-GPU shared setups
            if (renderer->CopyFrameToScanout(obs_tex)) {
                display_manager->PresentScanout();
            }
        }
    }
}

CleanOutputContext::CleanOutputContext() {
    for (size_t i = 0; i < MAX_DISPLAY_SLOTS; ++i) {
        slots_[i].index = static_cast<int>(i);
    }
}

CleanOutputContext::~CleanOutputContext() {
    Stop();
}

bool CleanOutputContext::Start() {
    if (active_ || stopping_) return false;
    stopping_ = false;

    bool any_started = false;
    for (auto &slot : slots_) {
        if (slot.enabled && slot.target_id != "none") {
            if (slot.Start()) {
                any_started = true;
            }
        }
    }

    if (!any_started) {
        blog(LOG_WARNING, "[GPU-Clean-Output] No active display slots to start.");
        return false;
    }

    active_ = true;
    obs_add_main_rendered_callback(clean_output_main_rendered, this);
    return true;
}

void CleanOutputContext::Stop() {
    stopping_ = true;

    obs_enter_graphics();
    obs_remove_main_rendered_callback(clean_output_main_rendered, this);
    obs_leave_graphics();

    for (auto &slot : slots_) {
        slot.Stop();
    }

    active_ = false;
    stopping_ = false;
}

void CleanOutputContext::UpdateSettings(obs_data_t *settings) {
    if (!settings) return;

    bool master_active = obs_data_get_bool(settings, "active");

    for (size_t i = 0; i < MAX_DISPLAY_SLOTS; ++i) {
        char target_key[32];
        char source_key[32];
        snprintf(target_key, sizeof(target_key), "out%zu_target", i + 1);
        snprintf(source_key, sizeof(source_key), "out%zu_source", i + 1);

        std::string target_id = "none";
        std::string source_type = "pgm";

        if (obs_data_has_user_value(settings, target_key)) {
            const char *val = obs_data_get_string(settings, target_key);
            if (val) target_id = val;
        } else if (i == 0 && obs_data_has_user_value(settings, "monitor_id")) {
            const char *val = obs_data_get_string(settings, "monitor_id");
            if (val) target_id = val;
        } else if (i == 0) {
            target_id = "auto";
        }

        if (obs_data_has_user_value(settings, source_key)) {
            const char *val = obs_data_get_string(settings, source_key);
            if (val) source_type = val;
        } else if (i == 0 && obs_data_has_user_value(settings, "source_type")) {
            const char *val = obs_data_get_string(settings, "source_type");
            if (val) source_type = val;
        }

        bool slot_enabled = master_active && (target_id != "none");
        bool slot_use_preview = (source_type == "pvw");

        bool config_changed = (slots_[i].target_id != target_id || slots_[i].use_preview != slot_use_preview || slots_[i].enabled != slot_enabled);

        slots_[i].target_id = target_id;
        slots_[i].use_preview = slot_use_preview;
        slots_[i].enabled = slot_enabled;

        if (config_changed && active_) {
            slots_[i].Stop();
            if (slots_[i].enabled) {
                slots_[i].Start();
            }
        }
    }

    bool should_be_active = false;
    for (auto &slot : slots_) {
        if (slot.enabled && slot.target_id != "none") {
            should_be_active = true;
            break;
        }
    }

    if (should_be_active && !active_ && !stopping_) {
        Start();
    } else if (!should_be_active && active_ && !stopping_) {
        Stop();
    }
}

void CleanOutputContext::RenderFrame() {
    if (!active_ || stopping_) return;

    obs_source_t *pvw_source = nullptr;
    bool needs_pvw = false;
    for (auto &slot : slots_) {
        if (slot.active && slot.use_preview) {
            needs_pvw = true;
            break;
        }
    }

    if (needs_pvw) {
        if (obs_frontend_preview_program_mode_active()) {
            pvw_source = obs_frontend_get_current_preview_scene();
        } else {
            pvw_source = obs_frontend_get_current_scene();
        }
    }

    try {
        for (auto &slot : slots_) {
            if (slot.active) {
                slot.Render(pvw_source);
            }
        }
    } catch (...) {
        blog(LOG_ERROR, "[GPU-Clean-Output] Exception in RenderFrame");
    }

    if (pvw_source) {
        obs_source_release(pvw_source);
    }
}

// C-API Wrappers for OBS Source & Output Registration
static const char *clean_output_get_name(void *) {
    return "GPU Clean Output (Specialized Display)";
}

static void *clean_source_create(obs_data_t *settings, obs_source_t *) {
    if (settings) {
        g_clean_output.UpdateSettings(settings);
    }
    return &g_clean_output;
}

static void clean_source_destroy(void *) {
    // Temporary Qt properties source destroyed by OBS UI — do NOT touch g_clean_output!
}

static void clean_output_update(void *, obs_data_t *settings) {
    if (settings) {
        g_clean_output.UpdateSettings(settings);
    }
}

static void clean_output_get_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, "active", false);
    obs_data_set_default_string(settings, "out1_target", "auto");
    obs_data_set_default_string(settings, "out1_source", "pgm");
    obs_data_set_default_string(settings, "out2_target", "none");
    obs_data_set_default_string(settings, "out2_source", "pgm");
    obs_data_set_default_string(settings, "out3_target", "none");
    obs_data_set_default_string(settings, "out3_source", "pgm");
    obs_data_set_default_string(settings, "out4_target", "none");
    obs_data_set_default_string(settings, "out4_source", "pgm");
}

static obs_properties_t *clean_output_get_properties(void *) {
    obs_properties_t *props = obs_properties_create();

    try {
        obs_properties_add_bool(props, "active", "Enable Direct Output (啟動硬體直輸出)");

        SpecialDisplayManager mgr;
        auto displays = mgr.EnumerateSpecializedDisplays();

        for (size_t i = 0; i < MAX_DISPLAY_SLOTS; ++i) {
            char target_key[32];
            char source_key[32];
            char target_label[64];
            char source_label[64];

            snprintf(target_key, sizeof(target_key), "out%zu_target", i + 1);
            snprintf(source_key, sizeof(source_key), "out%zu_source", i + 1);
            snprintf(target_label, sizeof(target_label), "Target Specialized Display %zu (目標螢幕 %zu)", i + 1, i + 1);
            snprintf(source_label, sizeof(source_label), "Source Output %zu (畫面訊號 %zu)", i + 1, i + 1);

            obs_property_t *monitor_list = obs_properties_add_list(
                props, target_key, target_label,
                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
            );

            obs_property_list_add_string(monitor_list, "None / Disabled (無輸出)", "none");
            obs_property_list_add_string(monitor_list, "Auto-Detect First Specialized Display", "auto");

            if (!displays.empty()) {
                for (auto const& disp : displays) {
                    obs_property_list_add_string(monitor_list, disp.name.c_str(), disp.id.c_str());
                }
            }

            obs_property_t *source_list = obs_properties_add_list(
                props, source_key, source_label,
                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
            );
            obs_property_list_add_string(source_list, "Program (PGM 主畫面)", "pgm");
            obs_property_list_add_string(source_list, "Preview (PVW 預覽畫面)", "pvw");
        }
    } catch (...) {}

    return props;
}

static uint32_t clean_source_get_width(void *) { return 1920; }
static uint32_t clean_source_get_height(void *) { return 1080; }

struct obs_output_info clean_output_info = [] {
    struct obs_output_info info = {};
    info.id = "d3d11_special_display_output";
    info.flags = OBS_OUTPUT_VIDEO;
    info.get_name = clean_output_get_name;
    info.create = [](obs_data_t *settings, obs_output_t *) -> void* {
        if (settings) g_clean_output.UpdateSettings(settings);
        return &g_clean_output;
    };
    info.destroy = [](void *) {};
    info.start = [](void *) -> bool { return g_clean_output.Start(); };
    info.stop = [](void *, uint64_t) { g_clean_output.Stop(); };
    info.update = clean_output_update;
    info.get_defaults = clean_output_get_defaults;
    info.get_properties = clean_output_get_properties;
    return info;
}();

struct obs_source_info clean_output_source_info = [] {
    struct obs_source_info info = {};
    info.id = "d3d11_special_display_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = clean_output_get_name;
    info.create = clean_source_create;
    info.destroy = clean_source_destroy;
    info.get_width = clean_source_get_width;
    info.get_height = clean_source_get_height;
    info.update = clean_output_update;
    info.get_defaults = clean_output_get_defaults;
    info.get_properties = clean_output_get_properties;
    return info;
}();
