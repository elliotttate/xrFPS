#define XRFPS_CORE_EXPORTS
#include "core_api.h"

#include <cmath>
#include <cstring>
#include <string>

#include "../gui/installer.h"
#include "../gui/metrics_collector.h"
#include "config.h"
#include "hud.h"
#include "stats.h"

namespace {

xrfps::MetricsCollector g_collector;

xrfps::Config from_abi(const XrFpsConfigAbi& in) {
    xrfps::Config c{};
    c.enabled = in.enabled != 0;
    c.hand = in.hand;
    c.flip_angle_deg = in.flip_angle_deg;
    c.show_delay_sec = in.show_delay_sec;
    c.hide_delay_sec = in.hide_delay_sec;
    c.f10_toggle = in.f10_toggle != 0;
    c.timing_source = in.timing_source;
    c.controller_placement = in.controller_placement;
    c.fallback_placement = in.fallback_placement;
    c.panel_width_m = in.panel_width_m;
    c.panel_offset_m = in.panel_offset_m;
    c.panel_roll_deg = in.panel_roll_deg;
    c.opacity = in.opacity;
    c.hud_refresh_hz = in.hud_refresh_hz;
    c.theme = in.theme;
    c.show_graph = in.show_graph != 0;
    c.graph_mode = in.graph_mode;
    c.graph_height = in.graph_height;
    c.show_fps = in.show_fps != 0;
    c.show_frametime = in.show_frametime != 0;
    c.show_cpu_frametime = in.show_cpu_frametime != 0;
    c.show_gpu_frametime = in.show_gpu_frametime != 0;
    c.show_target = in.show_target != 0;
    c.show_on_target = in.show_on_target != 0;
    c.show_percentiles = in.show_percentiles != 0;
    c.show_cpu_usage = in.show_cpu_usage != 0;
    c.show_gpu_usage = in.show_gpu_usage != 0;
    c.show_gpu_app_usage = in.show_gpu_app_usage != 0;
    c.show_vram = in.show_vram != 0;
    c.show_gpu_temp = in.show_gpu_temp != 0;
    c.show_cpu_temp = in.show_cpu_temp != 0;
    c.show_gpu_clock = in.show_gpu_clock != 0;
    c.show_ram = in.show_ram != 0;
    return c;
}

void to_abi(const xrfps::Config& c, XrFpsConfigAbi* out) {
    out->enabled = c.enabled;
    out->hand = c.hand;
    out->flip_angle_deg = c.flip_angle_deg;
    out->show_delay_sec = c.show_delay_sec;
    out->hide_delay_sec = c.hide_delay_sec;
    out->f10_toggle = c.f10_toggle;
    out->timing_source = c.timing_source;
    out->controller_placement = c.controller_placement;
    out->fallback_placement = c.fallback_placement;
    out->panel_width_m = c.panel_width_m;
    out->panel_offset_m = c.panel_offset_m;
    out->panel_roll_deg = c.panel_roll_deg;
    out->opacity = c.opacity;
    out->hud_refresh_hz = c.hud_refresh_hz;
    out->theme = c.theme;
    out->show_graph = c.show_graph;
    out->graph_mode = c.graph_mode;
    out->graph_height = c.graph_height;
    out->show_fps = c.show_fps;
    out->show_frametime = c.show_frametime;
    out->show_cpu_frametime = c.show_cpu_frametime;
    out->show_gpu_frametime = c.show_gpu_frametime;
    out->show_target = c.show_target;
    out->show_on_target = c.show_on_target;
    out->show_percentiles = c.show_percentiles;
    out->show_cpu_usage = c.show_cpu_usage;
    out->show_gpu_usage = c.show_gpu_usage;
    out->show_gpu_app_usage = c.show_gpu_app_usage;
    out->show_vram = c.show_vram;
    out->show_gpu_temp = c.show_gpu_temp;
    out->show_cpu_temp = c.show_cpu_temp;
    out->show_gpu_clock = c.show_gpu_clock;
    out->show_ram = c.show_ram;
}

void copy_string(const std::string& value, char* buffer, int32_t length) {
    if (buffer == nullptr || length <= 0) {
        return;
    }
    strncpy_s(buffer, (size_t)length, value.c_str(), _TRUNCATE);
}

} // namespace

void xrfps_config_defaults(XrFpsConfigAbi* out) {
    if (out != nullptr) {
        to_abi(xrfps::Config{}, out);
    }
}

int32_t xrfps_config_load(XrFpsConfigAbi* out) {
    if (out == nullptr) {
        return 0;
    }
    xrfps::Config c{};
    const bool ok = c.load();
    to_abi(c, out);
    return ok ? 1 : 0;
}

int32_t xrfps_config_save(const XrFpsConfigAbi* in) {
    return in != nullptr && from_abi(*in).save() ? 1 : 0;
}

void xrfps_config_path(char* buffer, int32_t length) {
    copy_string(xrfps::Config::path(), buffer, length);
}

int32_t xrfps_hud_width(void) { return HUD_W; }
int32_t xrfps_hud_height(void) { return HUD_H; }

void xrfps_hud_render(const XrFpsConfigAbi* config, int32_t use_live_metrics,
                        float preview_target_ms, uint8_t* out_bgra) {
    if (config == nullptr || out_bgra == nullptr) {
        return;
    }
    const float target = preview_target_ms > 0.5f ? preview_target_ms : 11.111f;

    // A plausible recent history so the preview reads like a live session.
    FrameStats stats{};
    stats.target_ms = target;
    stats.source = config->timing_source == 1
                       ? TimingBackend::SteamVR
                       : (config->timing_source == 2 ? TimingBackend::Meta
                                                     : TimingBackend::OpenXR);
    for (int i = 0; i < 320; ++i) {
        float ms = target * (0.94f + 0.05f * std::sin((float)i * 0.11f));
        if (i % 53 == 0) {
            ms += target * 1.1f;
        }
        if (i > 210 && i < 240) {
            ms += target * 0.45f;
        }
        const float cpu = target * (0.42f + 0.06f * std::sin((float)i * 0.07f));
        const float gpu = target * (0.72f + 0.08f * std::sin((float)i * 0.13f));
        stats.push(ms, cpu, gpu);
    }

    const xrfps::Config cfg = from_abi(*config);
    const xrfps::SharedMetrics& live = g_collector.latest();
    hud_render(out_bgra, stats, cfg, (use_live_metrics != 0 && g_collector.running()) ? &live : nullptr,
               /*bgra=*/true);
}

int32_t xrfps_status(char* manifest_path, int32_t length) {
    const auto s = xrfps::installer::query();
    copy_string(s.manifest_path, manifest_path, length);
    return (s.installed ? 1 : 0) | (s.files_present ? 2 : 0);
}

int32_t xrfps_install(char* error, int32_t length) {
    std::string message;
    const bool ok = xrfps::installer::install(&message);
    copy_string(message, error, length);
    return ok ? 1 : 0;
}

int32_t xrfps_uninstall(char* error, int32_t length) {
    std::string message;
    const bool ok = xrfps::installer::uninstall(&message);
    copy_string(message, error, length);
    return ok ? 1 : 0;
}

int32_t xrfps_metrics_start(void) { return g_collector.start() ? 1 : 0; }
void xrfps_metrics_stop(void) { g_collector.stop(); }
void xrfps_metrics_sample(void) { g_collector.sample(); }

void xrfps_metrics_get(XrFpsMetricsAbi* out) {
    if (out == nullptr) {
        return;
    }
    const xrfps::SharedMetrics& m = g_collector.latest();
    out->cpu_usage_pct = m.cpu_usage_pct;
    out->gpu_usage_pct = m.gpu_usage_pct;
    out->gpu_app_usage_pct = m.gpu_app_usage_pct;
    out->vram_used_mb = m.vram_used_mb;
    out->vram_total_mb = m.vram_total_mb;
    out->gpu_temp_c = m.gpu_temp_c;
    out->cpu_temp_c = m.cpu_temp_c;
    out->gpu_clock_mhz = m.gpu_clock_mhz;
    out->ram_used_gb = m.ram_used_gb;
    out->ram_total_gb = m.ram_total_gb;
    out->app_pid = m.app_pid;
    strncpy_s(out->app_name, sizeof(out->app_name), m.app_name, _TRUNCATE);
    out->app_fps = m.app_fps;
    out->app_frame_ms = m.app_frame_ms;
    out->app_cpu_frame_ms = m.app_cpu_frame_ms;
    out->app_gpu_frame_ms = m.app_gpu_frame_ms;
    out->timing_source = m.timing_source;
    out->requested_timing_source = m.requested_timing_source;
    strncpy_s(out->timing_source_name, sizeof(out->timing_source_name), m.timing_source_name,
              _TRUNCATE);
    out->running = g_collector.running() ? 1 : 0;
}

void xrfps_metrics_status(char* buffer, int32_t length) {
    copy_string(g_collector.status(), buffer, length);
}
