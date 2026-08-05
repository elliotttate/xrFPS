#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "font8x8_basic.h"

using xrfps::Config;
using xrfps::SharedMetrics;

namespace {

struct Color {
    uint8_t r, g, b, a;
};

struct Theme {
    Color panel, graph_bg, border, text, dim, good, warn, bad, target;
};

const Theme kThemes[] = {
    // Dark (default)
    {{18, 20, 26, 208}, {12, 13, 17, 228}, {60, 65, 78, 255}, {235, 238, 245, 255},
     {150, 158, 172, 255}, {86, 207, 133, 255}, {247, 181, 56, 255}, {240, 84, 84, 255},
     {110, 168, 254, 255}},
    // Light
    {{242, 244, 248, 220}, {255, 255, 255, 235}, {186, 192, 204, 255}, {24, 28, 36, 255},
     {104, 112, 128, 255}, {22, 138, 74, 255}, {176, 112, 12, 255}, {198, 40, 40, 255},
     {32, 96, 200, 255}},
    // High contrast
    {{0, 0, 0, 240}, {0, 0, 0, 250}, {255, 255, 255, 255}, {255, 255, 255, 255},
     {200, 200, 200, 255}, {0, 255, 0, 255}, {255, 255, 0, 255}, {255, 0, 0, 255},
     {0, 200, 255, 255}},
};

struct Canvas {
    uint8_t* px;
    bool bgra;
    float alpha;

    void put(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= HUD_W || y >= HUD_H) {
            return;
        }
        uint8_t* p = px + (y * HUD_W + x) * 4;
        if (bgra) {
            p[0] = c.b; p[1] = c.g; p[2] = c.r;
        } else {
            p[0] = c.r; p[1] = c.g; p[2] = c.b;
        }
        p[3] = (uint8_t)std::clamp((float)c.a * alpha, 0.0f, 255.0f);
    }

    void fill(int x0, int y0, int w, int h, Color c) {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                put(x, y, c);
            }
        }
    }

    void text(int x0, int y0, int scale, const char* s, Color c) {
        int x = x0;
        for (; *s != 0; ++s) {
            const unsigned ch = (unsigned char)*s;
            if (ch < 128) {
                const unsigned char* glyph = font8x8_basic[ch];
                for (int row = 0; row < 8; ++row) {
                    for (int col = 0; col < 8; ++col) {
                        if ((glyph[row] >> col) & 1) {
                            fill(x + col * scale, y0 + row * scale, scale, scale, c);
                        }
                    }
                }
            }
            x += 8 * scale;
        }
    }

    int text_w(const char* s, int scale) const { return (int)strlen(s) * 8 * scale; }
};

// Right-hand column of small "label value" rows, stacked downward.
struct RowStack {
    Canvas& cv;
    int right_x;
    int y;
    int scale;
    int line_h;

    void add(const char* text, Color c) {
        cv.text(right_x - cv.text_w(text, scale), y, scale, text, c);
        y += line_h;
    }
};

struct PendingRow {
    char text[32]{};
    Color color{};
};

} // namespace

void hud_render(uint8_t* out, const FrameStats& stats, const Config& config,
                const SharedMetrics* metrics, bool bgra) {
    const Theme& th = kThemes[std::clamp(config.theme, 0, 2)];
    Canvas cv{out, bgra, std::clamp(config.opacity, 0.05f, 1.0f)};

    std::memset(out, 0, HUD_W * HUD_H * 4);

    constexpr int kCorner = 10;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int cx = x < kCorner ? kCorner - x : (x >= HUD_W - kCorner ? x - (HUD_W - 1 - kCorner) : 0);
            const int cy = y < kCorner ? kCorner - y : (y >= HUD_H - kCorner ? y - (HUD_H - 1 - kCorner) : 0);
            if (cx * cx + cy * cy > kCorner * kCorner) {
                continue;
            }
            cv.put(x, y, th.panel);
        }
    }

    const float avg_ms = stats.avg();
    const float fps = stats.fps();
    const float target = stats.target_ms > 0.5f ? stats.target_ms : 11.111f;

    const auto severity = [&](float ms) {
        if (ms > target * 1.5f) {
            return th.bad;
        }
        if (ms > target * 1.06f) {
            return th.warn;
        }
        return th.good;
    };
    const auto load_color = [&](float pct) {
        if (pct >= 95.0f) {
            return th.bad;
        }
        if (pct >= 80.0f) {
            return th.warn;
        }
        return th.good;
    };

    char buf[64];

    // Headline FPS on the left.
    int header_bottom = 18;
    if (config.show_fps) {
        snprintf(buf, sizeof(buf), "%3.0f FPS", fps);
        cv.text(20, 18, 4, buf, severity(avg_ms));
        header_bottom = 18 + 8 * 4;
    }

    // Stacked readouts on the right.
    RowStack rows{cv, HUD_W - 18, 14, 2, 20};
    if (config.show_frametime) {
        snprintf(buf, sizeof(buf), "%5.2f MS", avg_ms);
        rows.add(buf, th.text);
    }
    if (config.show_target) {
        snprintf(buf, sizeof(buf), "TGT %4.1f", target);
        rows.add(buf, th.dim);
    }
    if (config.show_on_target && stats.count > 0) {
        int on_target = 0;
        for (int i = 0; i < stats.count; ++i) {
            if (stats.sample(i) <= target * 1.06f) {
                ++on_target;
            }
        }
        const int pct = (int)(100.0f * (float)on_target / (float)stats.count + 0.5f);
        snprintf(buf, sizeof(buf), "ON TGT %d%%", pct);
        rows.add(buf, pct >= 95 ? th.good : (pct >= 70 ? th.warn : th.bad));
    }
    if (config.show_percentiles && stats.count > 8) {
        snprintf(buf, sizeof(buf), "1%% LOW %3.0f", stats.percentile_fps(0.99f));
        rows.add(buf, th.dim);
    }

    // System metrics, when the Control Center is publishing them. Collect
    // them first, then render two columns beneath the headline rows. This
    // keeps the graph readable even when every available metric is enabled.
    PendingRow metric_rows[8]{};
    int metric_count = 0;
    const auto metric = [&](Color color, const char* format, auto... args) {
        if (metric_count >= (int)(sizeof(metric_rows) / sizeof(metric_rows[0]))) {
            return;
        }
        PendingRow& row = metric_rows[metric_count++];
        snprintf(row.text, sizeof(row.text), format, args...);
        row.color = color;
    };
    if (metrics != nullptr) {
        if (config.show_cpu_usage && metrics->cpu_usage_pct >= 0.0f) {
            metric(load_color(metrics->cpu_usage_pct), "CPU %3.0f%%", metrics->cpu_usage_pct);
        }
        if (config.show_gpu_usage && metrics->gpu_usage_pct >= 0.0f) {
            metric(load_color(metrics->gpu_usage_pct), "GPU %3.0f%%", metrics->gpu_usage_pct);
        }
        if (config.show_gpu_app_usage && metrics->gpu_app_usage_pct >= 0.0f) {
            metric(load_color(metrics->gpu_app_usage_pct), "APP %3.0f%%", metrics->gpu_app_usage_pct);
        }
        if (config.show_vram && metrics->vram_used_mb >= 0.0f) {
            metric(th.dim, "VRAM %.1fG", metrics->vram_used_mb / 1024.0f);
        }
        if (config.show_gpu_clock && metrics->gpu_clock_mhz >= 0.0f) {
            metric(th.dim, "CLK %4.0f", metrics->gpu_clock_mhz);
        }
        if (config.show_gpu_temp && metrics->gpu_temp_c >= 0.0f) {
            metric(metrics->gpu_temp_c >= 83.0f ? th.bad
                                                : (metrics->gpu_temp_c >= 75.0f ? th.warn : th.dim),
                   "GPU %2.0fC", metrics->gpu_temp_c);
        }
        if (config.show_cpu_temp && metrics->cpu_temp_c >= 0.0f) {
            metric(metrics->cpu_temp_c >= 90.0f ? th.bad
                                                : (metrics->cpu_temp_c >= 80.0f ? th.warn : th.dim),
                   "CPU %2.0fC", metrics->cpu_temp_c);
        }
        if (config.show_ram && metrics->ram_used_gb >= 0.0f) {
            metric(th.dim, "RAM %.1fG", metrics->ram_used_gb);
        }
    }

    const int metrics_y = std::max(header_bottom + 8, rows.y + 4);
    const int metrics_per_column = (metric_count + 1) / 2;
    RowStack metric_left{cv, HUD_W / 2 - 8, metrics_y, 2, 20};
    RowStack metric_right{cv, HUD_W - 18, metrics_y, 2, 20};
    for (int i = 0; i < metric_count; ++i) {
        RowStack& column = i < metrics_per_column ? metric_left : metric_right;
        column.add(metric_rows[i].text, metric_rows[i].color);
    }
    const int metrics_bottom = metrics_y + metrics_per_column * 20;

    if (!config.show_graph) {
        return;
    }

    // Frametime graph: one 2px bar per sample, newest at the right edge.
    const int gy = std::max({header_bottom + 8, rows.y + 4, metrics_bottom + 4});
    const int max_graph_h = HUD_H - gy - 12;
    if (max_graph_h < 24) {
        return;
    }
    const int gh = std::clamp(config.graph_height, 24, max_graph_h);
    constexpr int kGx = 16;
    constexpr int kGw = HUD_W - 32;
    constexpr int kBarW = 2;
    constexpr int kBars = kGw / kBarW;
    cv.fill(kGx - 1, gy - 1, kGw + 2, 1, th.border);
    cv.fill(kGx - 1, gy + gh, kGw + 2, 1, th.border);
    cv.fill(kGx - 1, gy, 1, gh, th.border);
    cv.fill(kGx + kGw, gy, 1, gh, th.border);
    cv.fill(kGx, gy, kGw, gh, th.graph_bg);

    // A stable target-relative scale makes consecutive frames comparable.
    // Very large spikes clip at the top rather than moving the target line.
    const float peak = target * 2.5f;
    const int avail = std::min(stats.count, kBars);

    const auto ms_to_y = [&](float m) {
        const float t = std::clamp(m / peak, 0.0f, 1.0f);
        return gy + gh - 1 - (int)(t * (float)(gh - 2));
    };

    const int ty = ms_to_y(target);
    for (int x = kGx; x < kGx + kGw; x += 10) {
        cv.fill(x, ty, 6, 1, th.target);
    }

    for (int i = 0; i < avail; ++i) {
        const float m = stats.sample(stats.count - avail + i);
        const int y = ms_to_y(m);
        const int x = kGx + kGw - (avail - i) * kBarW;
        cv.fill(x, y, kBarW, gy + gh - y, severity(m));
    }

    snprintf(buf, sizeof(buf), "TGT %.1fMS", target);
    cv.text(kGx + kGw - cv.text_w(buf, 1) - 4, ty - 11, 1, buf, th.target);
}
