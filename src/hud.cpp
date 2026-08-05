#include "hud.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "font8x8_basic.h"

using xrfps::Config;
using xrfps::SharedMetrics;

namespace {

struct Color {
    uint8_t r, g, b, a;
};

struct Theme {
    Color panel_top;
    Color panel_bottom;
    Color graph_bg;
    Color border;
    Color text;
    Color dim;
    Color good;
    Color warn;
    Color bad;
    Color target;
    Color accent;
    Color grid;
};

// The first three indices remain compatible with existing config files.
const Theme kThemes[] = {
    // Midnight — quiet graphite, soft violet and mint.
    {{13, 18, 30, 224}, {20, 27, 42, 224}, {8, 12, 22, 238}, {60, 65, 78, 255},
     {244, 247, 252, 255}, {151, 162, 182, 255}, {91, 219, 163, 255},
     {251, 190, 74, 255}, {255, 101, 119, 255}, {104, 202, 255, 255},
     {151, 116, 255, 255}, {41, 49, 67, 190}},
    // Porcelain — warm paper with ink, teal and cobalt.
    {{252, 249, 244, 236}, {238, 244, 247, 236}, {249, 251, 253, 246},
     {190, 199, 211, 255}, {28, 33, 43, 255}, {102, 112, 128, 255},
     {20, 148, 116, 255}, {201, 126, 24, 255}, {211, 61, 80, 255},
     {39, 103, 216, 255}, {111, 79, 194, 255}, {218, 225, 234, 220}},
    // High contrast — accessibility-first and deliberately unambiguous.
    {{0, 0, 0, 244}, {0, 0, 0, 244}, {0, 0, 0, 252}, {255, 255, 255, 255},
     {255, 255, 255, 255}, {205, 205, 205, 255}, {0, 255, 120, 255},
     {255, 225, 0, 255}, {255, 60, 60, 255}, {0, 210, 255, 255},
     {255, 255, 255, 255}, {100, 100, 100, 220}},
    // Aurora — deep polar night with violet and luminous cyan.
    {{7, 18, 36, 226}, {26, 13, 48, 226}, {5, 10, 27, 242}, {67, 78, 113, 255},
     {246, 248, 255, 255}, {157, 169, 204, 255}, {88, 236, 197, 255},
     {255, 199, 92, 255}, {255, 98, 135, 255}, {104, 216, 255, 255},
     {183, 121, 255, 255}, {36, 47, 82, 210}},
    // Ember — cinematic charcoal, copper and coral.
    {{34, 21, 20, 228}, {20, 18, 24, 228}, {16, 13, 17, 242}, {93, 67, 64, 255},
     {255, 246, 235, 255}, {190, 162, 151, 255}, {102, 219, 166, 255},
     {255, 184, 77, 255}, {255, 100, 91, 255}, {255, 159, 116, 255},
     {239, 105, 80, 255}, {68, 48, 48, 210}},
    // Glacier — deep blue glass with ice and periwinkle accents.
    {{8, 27, 42, 226}, {16, 40, 55, 226}, {5, 19, 31, 242}, {55, 91, 112, 255},
     {239, 250, 255, 255}, {145, 177, 195, 255}, {75, 224, 190, 255},
     {246, 202, 92, 255}, {255, 111, 128, 255}, {100, 211, 255, 255},
     {128, 156, 255, 255}, {29, 61, 78, 210}},
};

struct GdiState {
    ULONG_PTR token{};
    const wchar_t* family{L"Segoe UI"};
};

GdiState& gdi_state() {
    static GdiState state;
    static std::once_flag once;
    std::call_once(once, [&] {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&state.token, &input, nullptr) != Gdiplus::Ok) {
            state.token = 0;
            return;
        }
        Gdiplus::FontFamily variable(L"Segoe UI Variable Display");
        if (variable.GetLastStatus() == Gdiplus::Ok) {
            state.family = L"Segoe UI Variable Display";
        }
    });
    return state;
}

std::wstring widen(const char* text) {
    std::wstring result;
    while (*text != '\0') {
        result.push_back((wchar_t)(unsigned char)*text++);
    }
    return result;
}

Color mix(Color a, Color b, float t) {
    const auto channel = [t](uint8_t x, uint8_t y) {
        return (uint8_t)std::clamp((int)std::lround((float)x + ((float)y - (float)x) * t), 0, 255);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

struct Canvas {
    uint8_t* px;
    bool bgra;
    float alpha;
    Gdiplus::Bitmap* bitmap{};
    Gdiplus::Graphics* graphics{};

    Canvas(uint8_t* pixels, bool use_bgra, float opacity)
        : px(pixels), bgra(use_bgra), alpha(opacity) {
        if (gdi_state().token != 0) {
            bitmap = new Gdiplus::Bitmap(HUD_W, HUD_H, HUD_W * 4,
                                         PixelFormat32bppARGB, px);
            if (bitmap->GetLastStatus() == Gdiplus::Ok) {
                graphics = new Gdiplus::Graphics(bitmap);
                graphics->SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                graphics->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            }
        }
    }

    ~Canvas() {
        delete graphics;
        delete bitmap;
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void put(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= HUD_W || y >= HUD_H) {
            return;
        }
        uint8_t* p = px + (y * HUD_W + x) * 4;
        if (bgra) {
            p[0] = c.b;
            p[1] = c.g;
            p[2] = c.r;
        } else {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
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

    void line(int x0, int y0, int x1, int y1, Color c, int thickness = 1) {
        const int dx = std::abs(x1 - x0);
        const int sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0);
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            fill(x0 - thickness / 2, y0 - thickness / 2, thickness, thickness, c);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }

    static bool inside_round(int x, int y, int x0, int y0, int w, int h, int radius) {
        if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) {
            return false;
        }
        if (x >= x0 + radius && x < x0 + w - radius) {
            return true;
        }
        if (y >= y0 + radius && y < y0 + h - radius) {
            return true;
        }
        const int cx = x < x0 + radius ? x0 + radius : x0 + w - radius - 1;
        const int cy = y < y0 + radius ? y0 + radius : y0 + h - radius - 1;
        const int dx = x - cx;
        const int dy = y - cy;
        return dx * dx + dy * dy <= radius * radius;
    }

    void rounded_fill(int x0, int y0, int w, int h, int radius, Color c) {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if (inside_round(x, y, x0, y0, w, h, radius)) {
                    put(x, y, c);
                }
            }
        }
    }

    void rounded_gradient(int x0, int y0, int w, int h, int radius, Color top, Color bottom) {
        for (int y = y0; y < y0 + h; ++y) {
            const float t = h > 1 ? (float)(y - y0) / (float)(h - 1) : 0.0f;
            const Color row = mix(top, bottom, t);
            for (int x = x0; x < x0 + w; ++x) {
                if (inside_round(x, y, x0, y0, w, h, radius)) {
                    put(x, y, row);
                }
            }
        }
    }

    static int font_px(int scale) {
        if (scale >= 4) {
            return 34;
        }
        if (scale >= 2) {
            return 15;
        }
        return 10;
    }

    static int glyph_span(unsigned ch, int* first_col = nullptr) {
        if (ch == ' ') {
            if (first_col != nullptr) {
                *first_col = 0;
            }
            return 3;
        }
        int first = 8;
        int last = -1;
        if (ch < 128) {
            for (int row = 0; row < 8; ++row) {
                for (int col = 0; col < 8; ++col) {
                    if ((font8x8_basic[ch][row] >> col) & 1) {
                        first = std::min(first, col);
                        last = std::max(last, col);
                    }
                }
            }
        }
        if (first_col != nullptr) {
            *first_col = first == 8 ? 0 : first;
        }
        return last >= first ? last - first + 1 : 4;
    }

    void fallback_text(int x0, int y0, int scale, const char* text, Color c) {
        int x = x0;
        for (; *text != '\0'; ++text) {
            const unsigned ch = (unsigned char)*text;
            int first = 0;
            const int span = glyph_span(ch, &first);
            if (ch < 128) {
                for (int row = 0; row < 8; ++row) {
                    for (int col = first; col < first + span; ++col) {
                        if ((font8x8_basic[ch][row] >> col) & 1) {
                            fill(x + (col - first) * scale, y0 + row * scale, scale, scale, c);
                        }
                    }
                }
            }
            x += (span + 1) * scale;
        }
    }

    int fallback_text_w(const char* text, int scale) const {
        int width = 0;
        for (; *text != '\0'; ++text) {
            width += (glyph_span((unsigned char)*text) + 1) * scale;
        }
        return width;
    }

    void text(int x, int y, int scale, const char* value, Color c) {
        if (graphics == nullptr) {
            fallback_text(x, y, scale, value, c);
            return;
        }
        const std::wstring wide = widen(value);
        Gdiplus::FontFamily family(gdi_state().family);
        const int style = scale >= 2 ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
        Gdiplus::Font font(&family, (Gdiplus::REAL)font_px(scale), style, Gdiplus::UnitPixel);
        const BYTE red = bgra ? c.r : c.b;
        const BYTE blue = bgra ? c.b : c.r;
        Gdiplus::SolidBrush brush(Gdiplus::Color(
            (BYTE)std::clamp((float)c.a * alpha, 0.0f, 255.0f), red, c.g, blue));
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags() |
                              Gdiplus::StringFormatFlagsNoWrap |
                              Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        graphics->DrawString(wide.c_str(), -1, &font,
                             Gdiplus::PointF((Gdiplus::REAL)x, (Gdiplus::REAL)y),
                             &format, &brush);
    }

    int text_w(const char* value, int scale) {
        if (graphics == nullptr) {
            return fallback_text_w(value, scale);
        }
        const std::wstring wide = widen(value);
        Gdiplus::FontFamily family(gdi_state().family);
        const int style = scale >= 2 ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
        Gdiplus::Font font(&family, (Gdiplus::REAL)font_px(scale), style, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags() |
                              Gdiplus::StringFormatFlagsNoWrap |
                              Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        Gdiplus::RectF bounds;
        graphics->MeasureString(wide.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &bounds);
        return (int)std::ceil(bounds.Width);
    }
};

// Right-hand column of compact value rows, stacked downward.
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
    const Theme& th = kThemes[std::clamp(config.theme, 0, (int)(std::size(kThemes) - 1))];

    std::memset(out, 0, HUD_W * HUD_H * 4);
    Canvas cv{out, bgra, std::clamp(config.opacity, 0.05f, 1.0f)};

    cv.rounded_fill(0, 0, HUD_W, HUD_H, 13, th.border);
    cv.rounded_gradient(1, 1, HUD_W - 2, HUD_H - 2, 12, th.panel_top, th.panel_bottom);

    const float avg_ms = stats.avg();
    const float fps = stats.fps();
    const float cpu_ms = stats.cpu_avg();
    const float gpu_ms = stats.gpu_avg();
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

    // Large, calm headline typography with a small accent rail.
    int header_bottom = 18;
    if (config.show_fps) {
        cv.rounded_fill(16, 13, 5, 43, 3, th.accent);
        const char* headline = "OPENXR SUBMISSION RATE";
        if (stats.source == TimingBackend::SteamVR) {
            headline = "STEAMVR APP FRAME RATE";
        } else if (stats.source == TimingBackend::Meta) {
            headline = "META + OPENXR APP RATE";
        }
        cv.text(29, 9, 1, headline, th.dim);
        snprintf(buf, sizeof(buf), "%.0f", fps);
        cv.text(27, 22, 4, buf, severity(avg_ms));
        const int number_width = cv.text_w(buf, 4);
        cv.text(33 + number_width, 37, 2, "FPS", th.dim);
        header_bottom = 63;
    }

    // Stacked values remain compact so the graph keeps useful vertical space.
    RowStack rows{cv, HUD_W - 20, 12, 2, 20};
    if (config.show_frametime) {
        snprintf(buf, sizeof(buf), "Frame %.2f ms", avg_ms);
        rows.add(buf, th.text);
    }
    if (config.show_cpu_frametime && cpu_ms > 0.0f) {
        snprintf(buf, sizeof(buf), stats.source == TimingBackend::OpenXR ? "CPU span %.2f ms"
                                                                        : "CPU %.2f ms",
                 cpu_ms);
        rows.add(buf, th.target);
    }
    if (config.show_gpu_frametime && gpu_ms > 0.0f) {
        snprintf(buf, sizeof(buf), "GPU %.2f ms", gpu_ms);
        rows.add(buf, th.accent);
    }
    if (config.show_target) {
        snprintf(buf, sizeof(buf), "Target %.1f", target);
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
        snprintf(buf, sizeof(buf), "%d%% on target", pct);
        rows.add(buf, pct >= 95 ? th.good : (pct >= 70 ? th.warn : th.bad));
    }
    if (config.show_percentiles && stats.count > 8) {
        snprintf(buf, sizeof(buf), "1%% low %.0f", stats.percentile_fps(0.99f));
        rows.add(buf, th.dim);
    }

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
            metric(load_color(metrics->cpu_usage_pct), "CPU %.0f%%", metrics->cpu_usage_pct);
        }
        if (config.show_gpu_usage && metrics->gpu_usage_pct >= 0.0f) {
            metric(load_color(metrics->gpu_usage_pct), "GPU %.0f%%", metrics->gpu_usage_pct);
        }
        if (config.show_gpu_app_usage && metrics->gpu_app_usage_pct >= 0.0f) {
            metric(load_color(metrics->gpu_app_usage_pct), "App %.0f%%", metrics->gpu_app_usage_pct);
        }
        if (config.show_vram && metrics->vram_used_mb >= 0.0f) {
            metric(th.dim, "VRAM %.1f GB", metrics->vram_used_mb / 1024.0f);
        }
        if (config.show_gpu_clock && metrics->gpu_clock_mhz >= 0.0f) {
            metric(th.dim, "Clock %.0f MHz", metrics->gpu_clock_mhz);
        }
        if (config.show_gpu_temp && metrics->gpu_temp_c >= 0.0f) {
            metric(metrics->gpu_temp_c >= 83.0f ? th.bad
                                                : (metrics->gpu_temp_c >= 75.0f ? th.warn : th.dim),
                   "GPU %.0f C", metrics->gpu_temp_c);
        }
        if (config.show_cpu_temp && metrics->cpu_temp_c >= 0.0f) {
            metric(metrics->cpu_temp_c >= 90.0f ? th.bad
                                                : (metrics->cpu_temp_c >= 80.0f ? th.warn : th.dim),
                   "CPU %.0f C", metrics->cpu_temp_c);
        }
        if (config.show_ram && metrics->ram_used_gb >= 0.0f) {
            metric(th.dim, "RAM %.1f GB", metrics->ram_used_gb);
        }
    }

    const int metrics_y = std::max(header_bottom + 5, rows.y + 2);
    const int metrics_per_column = (metric_count + 1) / 2;
    RowStack metric_left{cv, HUD_W / 2 - 10, metrics_y, 2, 19};
    RowStack metric_right{cv, HUD_W - 20, metrics_y, 2, 19};
    for (int i = 0; i < metric_count; ++i) {
        RowStack& column = i < metrics_per_column ? metric_left : metric_right;
        column.add(metric_rows[i].text, metric_rows[i].color);
    }
    const int metrics_bottom = metrics_y + metrics_per_column * 19;

    if (!config.show_graph) {
        return;
    }

    const int gy = std::max({header_bottom + 5, rows.y + 2, metrics_bottom + 3});
    const int max_graph_h = HUD_H - gy - 12;
    const bool separate_graphs = config.graph_mode == 3;
    constexpr int min_graph_h = 24;
    if (max_graph_h < min_graph_h) {
        return;
    }
    const int gh = std::clamp(config.graph_height, min_graph_h, max_graph_h);
    constexpr int kGx = 16;
    constexpr int kGw = HUD_W - 32;
    constexpr int kInset = 7;
    constexpr int kBarW = 2;

    // A stable target-relative scale keeps consecutive frames comparable.
    const float peak = target * 2.5f;
    const auto ms_to_y = [&](float ms, int panel_y, int panel_h) {
        const float t = std::clamp(ms / peak, 0.0f, 1.0f);
        return panel_y + panel_h - 5 - (int)(t * (float)(panel_h - 10));
    };
    const auto draw_panel = [&](int panel_x, int panel_y, int panel_w, int panel_h) {
        cv.rounded_fill(panel_x - 1, panel_y - 1, panel_w + 2, panel_h + 2, 9, th.border);
        cv.rounded_fill(panel_x, panel_y, panel_w, panel_h, 8, th.graph_bg);
        const int plot_x = panel_x + kInset;
        const int plot_w = panel_w - kInset * 2;
        for (int grid = 1; grid <= 3; ++grid) {
            const int y = panel_y + (panel_h * grid) / 4;
            cv.fill(plot_x, y, plot_w, 1, th.grid);
        }
    };
    const auto draw_target = [&](int panel_x, int panel_y, int panel_w, int panel_h) {
        const int plot_x = panel_x + kInset;
        const int plot_w = panel_w - kInset * 2;
        const int ty = ms_to_y(target, panel_y, panel_h);
        for (int x = plot_x; x < plot_x + plot_w; x += 12) {
            cv.fill(x, ty, 7, 1, th.target);
        }
        return ty;
    };
    const auto draw_trace = [&](int panel_x, int panel_y, int panel_w, int panel_h, Color color,
                                bool cpu) {
        const int plot_x = panel_x + kInset;
        const int plot_w = panel_w - kInset * 2;
        const int avail = std::min(stats.count, plot_w / kBarW);
        int previous_x = 0;
        int previous_y = 0;
        bool previous = false;
        for (int i = 0; i < avail; ++i) {
            const int sample_index = stats.count - avail + i;
            const int x = plot_x + plot_w - (avail - i) * kBarW + kBarW / 2;
            const float value = cpu ? stats.cpu_sample(sample_index)
                                    : stats.gpu_sample(sample_index);
            if (value > 0.0f) {
                const int y = ms_to_y(value, panel_y, panel_h);
                if (previous) {
                    cv.line(previous_x, previous_y, x, y, color, 2);
                }
                previous_y = y;
                previous = true;
            } else {
                previous = false;
            }
            previous_x = x;
        }
    };
    const auto draw_panel_label = [&](int panel_x, int panel_y, const char* name, float value,
                                      Color color) {
        char label[32];
        if (value > 0.0f) {
            snprintf(label, sizeof(label), "%s %.2f ms", name, value);
        } else {
            snprintf(label, sizeof(label), "%s unavailable", name);
        }
        const int label_w = cv.text_w(label, 1);
        const int plot_x = panel_x + kInset;
        cv.rounded_fill(plot_x + 3, panel_y + 3, label_w + 22, 17, 6, th.graph_bg);
        cv.fill(plot_x + 9, panel_y + 10, 10, 2, color);
        cv.text(plot_x + 24, panel_y + 4, 1, label, value > 0.0f ? color : th.dim);
    };

    if (separate_graphs) {
        constexpr int kGap = 7;
        const int cpu_w = (kGw - kGap) / 2;
        const int gpu_x = kGx + cpu_w + kGap;
        const int gpu_w = kGx + kGw - gpu_x;

        draw_panel(kGx, gy, cpu_w, gh);
        draw_panel(gpu_x, gy, gpu_w, gh);
        draw_trace(kGx, gy, cpu_w, gh, th.target, true);
        draw_trace(gpu_x, gy, gpu_w, gh, th.accent, false);
        draw_target(kGx, gy, cpu_w, gh);
        draw_target(gpu_x, gy, gpu_w, gh);
        draw_panel_label(kGx, gy, "CPU", cpu_ms, th.target);
        draw_panel_label(gpu_x, gy, "GPU", gpu_ms, th.accent);
        return;
    }

    constexpr int kPlotX = kGx + kInset;
    constexpr int kPlotW = kGw - kInset * 2;
    constexpr int kBars = kPlotW / kBarW;
    const int avail = std::min(stats.count, kBars);
    draw_panel(kGx, gy, kGw, gh);
    const bool combined_graph = config.graph_mode == 2 ||
                                (config.graph_mode == 0 && stats.has_cpu() && stats.has_gpu());
    if (!combined_graph) {
        for (int i = 0; i < avail; ++i) {
            const float value = stats.sample(stats.count - avail + i);
            const int y = ms_to_y(value, gy, gh);
            const int x = kPlotX + kPlotW - (avail - i) * kBarW;
            cv.fill(x, y, kBarW, gy + gh - 5 - y, severity(value));
        }
    } else {
        draw_trace(kGx, gy, kGw, gh, th.target, true);
        draw_trace(kGx, gy, kGw, gh, th.accent, false);
        cv.rounded_fill(kPlotX + 3, gy + 3, 76, 18, 6, th.graph_bg);
        cv.fill(kPlotX + 9, gy + 9, 12, 2, th.target);
        cv.text(kPlotX + 25, gy + 4, 1, "CPU", th.target);
        cv.fill(kPlotX + 48, gy + 9, 12, 2, th.accent);
        cv.text(kPlotX + 64, gy + 4, 1, "GPU", th.accent);
    }

    const int ty = draw_target(kGx, gy, kGw, gh);

    snprintf(buf, sizeof(buf), "Target %.1f ms", target);
    const int label_width = cv.text_w(buf, 1);
    const int label_x = kPlotX + kPlotW - label_width - 3;
    const int label_y = std::clamp(ty - 13, gy + 2, gy + gh - 15);
    cv.rounded_fill(label_x - 4, label_y - 1, label_width + 8, 14, 6, th.graph_bg);
    cv.text(label_x, label_y, 1, buf, th.target);
}
