#pragma once

#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "special-display-manager.hpp"
#include "d3d11-output-renderer.hpp"

struct DisplayOutputSlot {
    int index{ 0 };
    bool enabled{ false };
    bool use_preview{ false };
    std::string target_id{ "none" };
    bool active{ false };
    std::atomic<bool> is_degraded{ false };
    uint64_t last_reconnect_time{ 0 };

    std::unique_ptr<SpecialDisplayManager> display_manager;
    std::unique_ptr<D3D11OutputRenderer> renderer;
    gs_texrender_t *pvw_texrender{ nullptr };
    obs_source_t *pvw_showing_source{ nullptr };

    // Producer-Consumer Background Worker Thread
    std::thread worker_thread;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> thread_running{ false };
    std::atomic<int> pending_frames{ 0 };

    DisplayOutputSlot();
    ~DisplayOutputSlot();

    bool Start();
    void Stop();
    void UpdateShowingSource(obs_source_t *new_source);
    void Render(obs_source_t *pvw_source);

private:
    void WorkerThreadLoop();
    void StopPipelineInternal();
};

constexpr size_t MAX_DISPLAY_SLOTS = 4;

class CleanOutputContext {
public:
    CleanOutputContext();
    ~CleanOutputContext();

    bool Start();
    void Stop();
    void UpdateSettings(obs_data_t *settings);
    void RenderFrame();

    bool IsActive() const { return active_; }

private:
    DisplayOutputSlot slots_[MAX_DISPLAY_SLOTS];

    bool active_{ false };
    bool stopping_{ false };
};

extern CleanOutputContext g_clean_output;

extern struct obs_output_info clean_output_info;
extern struct obs_source_info clean_output_source_info;
