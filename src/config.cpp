#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xrfps {
namespace {

std::string local_appdata_dir() {
    char buf[MAX_PATH]{};
    if (GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf)) == 0) {
        return {};
    }
    std::string dir = std::string{buf} + "\\xrFPS";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

void trim(std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        s.clear();
        return;
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
}

} // namespace

std::string Config::path() {
    const std::string dir = local_appdata_dir();
    return dir.empty() ? std::string{} : dir + "\\config.txt";
}

bool Config::load(const std::string& file) {
    FILE* f = nullptr;
    if (fopen_s(&f, file.c_str(), "r") != 0 || f == nullptr) {
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string text{line};
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const auto eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = text.substr(0, eq);
        std::string value = text.substr(eq + 1);
        trim(key);
        trim(value);
        if (key.empty() || value.empty()) {
            continue;
        }

        const auto as_bool = [&] { return value == "1" || value == "true"; };
        const auto as_float = [&] { return (float)atof(value.c_str()); };
        const auto as_int = [&] { return atoi(value.c_str()); };

        if (key == "enabled") { enabled = as_bool(); }
        else if (key == "hand") { hand = as_int(); }
        else if (key == "flip_angle_deg") { flip_angle_deg = as_float(); }
        else if (key == "show_delay_sec") { show_delay_sec = as_float(); }
        else if (key == "hide_delay_sec") { hide_delay_sec = as_float(); }
        else if (key == "f10_toggle") { f10_toggle = as_bool(); }
        else if (key == "timing_source") { timing_source = as_int(); }
        else if (key == "panel_width_m") { panel_width_m = as_float(); }
        else if (key == "panel_offset_m") { panel_offset_m = as_float(); }
        else if (key == "panel_roll_deg") { panel_roll_deg = as_float(); }
        else if (key == "opacity") { opacity = as_float(); }
        else if (key == "hud_refresh_hz") { hud_refresh_hz = as_float(); }
        else if (key == "theme") { theme = as_int(); }
        else if (key == "show_graph") { show_graph = as_bool(); }
        else if (key == "graph_mode") { graph_mode = as_int(); }
        else if (key == "graph_height") { graph_height = as_int(); }
        else if (key == "show_fps") { show_fps = as_bool(); }
        else if (key == "show_frametime") { show_frametime = as_bool(); }
        else if (key == "show_cpu_frametime") { show_cpu_frametime = as_bool(); }
        else if (key == "show_gpu_frametime") { show_gpu_frametime = as_bool(); }
        else if (key == "show_target") { show_target = as_bool(); }
        else if (key == "show_on_target") { show_on_target = as_bool(); }
        else if (key == "show_percentiles") { show_percentiles = as_bool(); }
        else if (key == "show_cpu_usage") { show_cpu_usage = as_bool(); }
        else if (key == "show_gpu_usage") { show_gpu_usage = as_bool(); }
        else if (key == "show_gpu_app_usage") { show_gpu_app_usage = as_bool(); }
        else if (key == "show_vram") { show_vram = as_bool(); }
        else if (key == "show_gpu_temp") { show_gpu_temp = as_bool(); }
        else if (key == "show_cpu_temp") { show_cpu_temp = as_bool(); }
        else if (key == "show_gpu_clock") { show_gpu_clock = as_bool(); }
        else if (key == "show_ram") { show_ram = as_bool(); }
    }
    fclose(f);
    return true;
}

bool Config::save(const std::string& file) const {
    // Write to a temp file and move it into place: the layer polls this file
    // from another process and must never observe a half-written config.
    const std::string temp = file + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, temp.c_str(), "w") != 0 || f == nullptr) {
        return false;
    }

    fprintf(f, "# xrFPS configuration - written by the Control Center.\n");
    fprintf(f, "# Changes apply live; no need to restart the game.\n\n");
    fprintf(f, "enabled = %d\n", enabled ? 1 : 0);
    fprintf(f, "hand = %d\n", hand);
    fprintf(f, "flip_angle_deg = %.1f\n", flip_angle_deg);
    fprintf(f, "show_delay_sec = %.2f\n", show_delay_sec);
    fprintf(f, "hide_delay_sec = %.2f\n", hide_delay_sec);
    fprintf(f, "f10_toggle = %d\n", f10_toggle ? 1 : 0);
    fprintf(f, "timing_source = %d\n", timing_source);
    fprintf(f, "\npanel_width_m = %.3f\n", panel_width_m);
    fprintf(f, "panel_offset_m = %.3f\n", panel_offset_m);
    fprintf(f, "panel_roll_deg = %.1f\n", panel_roll_deg);
    fprintf(f, "\nopacity = %.2f\n", opacity);
    fprintf(f, "hud_refresh_hz = %.1f\n", hud_refresh_hz);
    fprintf(f, "theme = %d\n", theme);
    fprintf(f, "show_graph = %d\n", show_graph ? 1 : 0);
    fprintf(f, "graph_mode = %d\n", graph_mode);
    fprintf(f, "graph_height = %d\n", graph_height);
    fprintf(f, "\nshow_fps = %d\n", show_fps ? 1 : 0);
    fprintf(f, "show_frametime = %d\n", show_frametime ? 1 : 0);
    fprintf(f, "show_cpu_frametime = %d\n", show_cpu_frametime ? 1 : 0);
    fprintf(f, "show_gpu_frametime = %d\n", show_gpu_frametime ? 1 : 0);
    fprintf(f, "show_target = %d\n", show_target ? 1 : 0);
    fprintf(f, "show_on_target = %d\n", show_on_target ? 1 : 0);
    fprintf(f, "show_percentiles = %d\n", show_percentiles ? 1 : 0);
    fprintf(f, "show_cpu_usage = %d\n", show_cpu_usage ? 1 : 0);
    fprintf(f, "show_gpu_usage = %d\n", show_gpu_usage ? 1 : 0);
    fprintf(f, "show_gpu_app_usage = %d\n", show_gpu_app_usage ? 1 : 0);
    fprintf(f, "show_vram = %d\n", show_vram ? 1 : 0);
    fprintf(f, "show_gpu_temp = %d\n", show_gpu_temp ? 1 : 0);
    fprintf(f, "show_cpu_temp = %d\n", show_cpu_temp ? 1 : 0);
    fprintf(f, "show_gpu_clock = %d\n", show_gpu_clock ? 1 : 0);
    fprintf(f, "show_ram = %d\n", show_ram ? 1 : 0);
    fclose(f);

    return MoveFileExA(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

unsigned long long Config::stamp(const std::string& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(file.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

} // namespace xrfps
