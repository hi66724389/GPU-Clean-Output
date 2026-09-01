#include <obs-module.h>
#include <obs-frontend-api.h>
#include "clean-output-output.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("gpu-clean-output", "en-US")

static obs_source_t *g_settings_source = nullptr;

static void on_tools_menu_clicked(void *private_data) {
    UNUSED_PARAMETER(private_data);
    
    if (!g_settings_source) {
        obs_data_t *settings = obs_data_create();
        g_settings_source = obs_source_create_private(
            "d3d11_special_display_source",
            "GPU Clean Output Settings",
            settings
        );
        obs_data_release(settings);
    }

    if (g_settings_source) {
        obs_frontend_open_source_properties(g_settings_source);
    }
}


static void on_frontend_event(enum obs_frontend_event event, void *private_data) {
    UNUSED_PARAMETER(private_data);
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        blog(LOG_INFO, "[GPU-Clean-Output] OBS frontend exit event received. Stopping all display outputs...");
        g_clean_output.Stop();
    }
}

MODULE_EXPORT bool obs_module_load(void) {
    blog(LOG_INFO, "[GPU-Clean-Output] Loading GPU Clean Output Plugin v1.0.0...");

    obs_register_output(&clean_output_info);
    obs_register_source(&clean_output_source_info);

    obs_frontend_add_tools_menu_item(
        "GPU Clean Output (Specialized Display)",
        on_tools_menu_clicked,
        nullptr
    );

    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    blog(LOG_INFO, "[GPU-Clean-Output] Plugin loaded successfully!");
    return true;
}

MODULE_EXPORT void obs_module_unload(void) {
    blog(LOG_INFO, "[GPU-Clean-Output] Unloading GPU Clean Output Plugin...");
    
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    g_clean_output.Stop();

    if (g_settings_source) {
        obs_source_release(g_settings_source);
        g_settings_source = nullptr;
    }
}
