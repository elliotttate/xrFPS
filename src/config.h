#pragma once

#include <string>

namespace xrfps {

// Every user-facing setting. Shared by the layer (reads, live) and the Control
// Center (reads/writes). Kept plain so both sides can use it without any
// serialization dependency.
struct Config {
    // --- behavior ---
    bool enabled{true};
    // 0 = whichever controller is flipped, 1 = left only, 2 = right only.
    int hand{0};
    // Degrees past upright the grip must rotate to count as "upside down".
    float flip_angle_deg{120.0f};
    float show_delay_sec{0.25f};
    float hide_delay_sec{0.5f};
    bool f10_toggle{true};

    // --- placement ---
    float panel_width_m{0.32f};
    float panel_offset_m{0.11f};
    // Extra roll of the panel about the controller axis, degrees.
    float panel_roll_deg{0.0f};

    // --- appearance ---
    float opacity{0.9f};
    float hud_refresh_hz{6.7f};
    // 0 = Midnight, 1 = Porcelain, 2 = high contrast,
    // 3 = Aurora, 4 = Ember, 5 = Glacier.
    int theme{0};
    bool show_graph{true};
    int graph_height{168};

    // --- rows ---
    bool show_fps{true};
    bool show_frametime{true};
    bool show_target{true};
    bool show_on_target{true};
    bool show_percentiles{false}; // 1% low / 0.1% low
    bool show_cpu_usage{false};
    bool show_gpu_usage{false};
    bool show_gpu_app_usage{false}; // this app's share of the GPU
    bool show_vram{false};
    bool show_gpu_temp{false};
    bool show_cpu_temp{false};
    bool show_gpu_clock{false};
    bool show_ram{false};

    // Where the config lives: %LOCALAPPDATA%\xrFPS\config.txt
    static std::string path();

    // Missing keys keep their defaults, so new options roll out cleanly.
    bool load(const std::string& file = path());
    bool save(const std::string& file = path()) const;

    // Cheap change check for live reload (file write time, 0 if absent).
    static unsigned long long stamp(const std::string& file = path());
};

} // namespace xrfps
