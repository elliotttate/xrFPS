#pragma once

// Flat C ABI over the layer's own configuration, HUD renderer, installer and
// metrics collector, so the Control Center can drive the real implementations
// instead of reimplementing them.
#include <stdint.h>

#if defined(XRFPS_CORE_EXPORTS)
#define XRFPS_API __declspec(dllexport)
#else
#define XRFPS_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// int32 rather than bool throughout: no ABI surprises when marshalling.
typedef struct XrFpsConfigAbi {
    int32_t enabled;
    int32_t hand;
    float flip_angle_deg;
    float show_delay_sec;
    float hide_delay_sec;
    int32_t f10_toggle;

    float panel_width_m;
    float panel_offset_m;
    float panel_roll_deg;

    float opacity;
    float hud_refresh_hz;
    int32_t theme;
    int32_t show_graph;
    int32_t graph_height;

    int32_t show_fps;
    int32_t show_frametime;
    int32_t show_target;
    int32_t show_on_target;
    int32_t show_percentiles;
    int32_t show_cpu_usage;
    int32_t show_gpu_usage;
    int32_t show_gpu_app_usage;
    int32_t show_vram;
    int32_t show_gpu_temp;
    int32_t show_cpu_temp;
    int32_t show_gpu_clock;
    int32_t show_ram;
} XrFpsConfigAbi;

typedef struct XrFpsMetricsAbi {
    float cpu_usage_pct;
    float gpu_usage_pct;
    float gpu_app_usage_pct;
    float vram_used_mb;
    float vram_total_mb;
    float gpu_temp_c;
    float cpu_temp_c;
    float gpu_clock_mhz;
    float ram_used_gb;
    float ram_total_gb;
    uint32_t app_pid;
    char app_name[64];
    int32_t running;
} XrFpsMetricsAbi;

// --- configuration ---
XRFPS_API void xrfps_config_defaults(XrFpsConfigAbi* out);
XRFPS_API int32_t xrfps_config_load(XrFpsConfigAbi* out);
XRFPS_API int32_t xrfps_config_save(const XrFpsConfigAbi* in);
XRFPS_API void xrfps_config_path(char* buffer, int32_t length);

// --- HUD preview (the layer's own rasterizer) ---
XRFPS_API int32_t xrfps_hud_width(void);
XRFPS_API int32_t xrfps_hud_height(void);
// Writes width*height*4 BGRA bytes. When `use_live_metrics` is non-zero the
// collector's latest sample fills the system rows.
XRFPS_API void xrfps_hud_render(const XrFpsConfigAbi* config, int32_t use_live_metrics,
                                    float preview_target_ms, uint8_t* out_bgra);

// --- install / uninstall ---
// Returns a bitmask: 1 = registered, 2 = layer files present.
XRFPS_API int32_t xrfps_status(char* manifest_path, int32_t length);
XRFPS_API int32_t xrfps_install(char* error, int32_t length);
XRFPS_API int32_t xrfps_uninstall(char* error, int32_t length);

// --- metrics collector ---
XRFPS_API int32_t xrfps_metrics_start(void);
XRFPS_API void xrfps_metrics_stop(void);
XRFPS_API void xrfps_metrics_sample(void);
XRFPS_API void xrfps_metrics_get(XrFpsMetricsAbi* out);
XRFPS_API void xrfps_metrics_status(char* buffer, int32_t length);

#ifdef __cplusplus
}
#endif
