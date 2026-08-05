#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "font8x8_basic.h"

namespace {

struct Color {
    uint8_t r, g, b, a;
};

constexpr Color kPanelBg{18, 20, 26, 208};
constexpr Color kGraphBg{12, 13, 17, 228};
constexpr Color kGraphBorder{60, 65, 78, 255};
constexpr Color kText{235, 238, 245, 255};
constexpr Color kTextDim{150, 158, 172, 255};
constexpr Color kGood{86, 207, 133, 255};
constexpr Color kWarn{247, 181, 56, 255};
constexpr Color kBad{240, 84, 84, 255};
constexpr Color kTarget{110, 168, 254, 255};

struct Canvas {
    uint8_t* px;
    bool bgra;

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
        p[3] = c.a;
    }

    void fill(int x0, int y0, int w, int h, Color c) {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                put(x, y, c);
            }
        }
    }

    // 8x8 bitmap font scaled by an integer factor.
    void text(int x0, int y0, int scale, const char* s, Color c) {
        int x = x0;
        for (; *s != 0; ++s) {
            const unsigned ch = (unsigned char)*s;
            if (ch < 128) {
                const char* glyph = font8x8_basic[ch];
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
};

} // namespace

void hud_render(uint8_t* out, const FrameStats& stats, bool bgra) {
    Canvas cv{out, bgra};

    std::memset(out, 0, HUD_W * HUD_H * 4);

    // Panel background with clipped corners.
    constexpr int kCorner = 10;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int cx = x < kCorner ? kCorner - x : (x >= HUD_W - kCorner ? x - (HUD_W - 1 - kCorner) : 0);
            const int cy = y < kCorner ? kCorner - y : (y >= HUD_H - kCorner ? y - (HUD_H - 1 - kCorner) : 0);
            if (cx * cx + cy * cy > kCorner * kCorner) {
                continue;
            }
            cv.put(x, y, kPanelBg);
        }
    }

    const float avg_ms = stats.avg();
    const float fps = stats.fps();
    const float target = stats.target_ms > 0.5f ? stats.target_ms : 11.111f;

    Color fps_color = kGood;
    if (avg_ms > target * 1.5f) {
        fps_color = kBad;
    } else if (avg_ms > target * 1.06f) {
        fps_color = kWarn;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%3.0f FPS", fps);
    cv.text(20, 18, 4, buf, fps_color);

    snprintf(buf, sizeof(buf), "%5.2f MS", avg_ms);
    cv.text(HUD_W - 20 - 8 * 2 * (int)strlen(buf), 14, 2, buf, kText);

    snprintf(buf, sizeof(buf), "TGT %4.1f", target);
    cv.text(HUD_W - 20 - 8 * 2 * (int)strlen(buf), 34, 2, buf, kTextDim);

    // Share of recent frames that hit the target — makes "peak rate vs real
    // delivered rate" visible at a glance.
    if (stats.count > 0) {
        int on_target = 0;
        for (int i = 0; i < stats.count; ++i) {
            if (stats.sample(i) <= target * 1.06f) {
                ++on_target;
            }
        }
        const int pct = (int)(100.0f * (float)on_target / (float)stats.count + 0.5f);
        Color c = pct >= 95 ? kGood : (pct >= 70 ? kWarn : kBad);
        snprintf(buf, sizeof(buf), "ON TGT %d%%", pct);
        cv.text(HUD_W - 20 - 8 * 2 * (int)strlen(buf), 54, 2, buf, c);
    }

    // Frametime graph: one 2px bar per sample, newest at the right edge.
    constexpr int kGx = 16, kGy = 70, kGw = 480, kGh = 168;
    constexpr int kBarW = 2;
    constexpr int kBars = kGw / kBarW;

    cv.fill(kGx - 1, kGy - 1, kGw + 2, 1, kGraphBorder);
    cv.fill(kGx - 1, kGy + kGh, kGw + 2, 1, kGraphBorder);
    cv.fill(kGx - 1, kGy, 1, kGh, kGraphBorder);
    cv.fill(kGx + kGw, kGy, 1, kGh, kGraphBorder);
    cv.fill(kGx, kGy, kGw, kGh, kGraphBg);

    float peak = target * 2.2f;
    const int avail = std::min(stats.count, kBars);
    for (int i = 0; i < avail; ++i) {
        peak = std::max(peak, stats.sample(stats.count - avail + i) * 1.1f);
    }

    const auto ms_to_y = [&](float m) {
        const float t = std::clamp(m / peak, 0.0f, 1.0f);
        return kGy + kGh - 1 - (int)(t * (float)(kGh - 2));
    };

    // Dashed target line.
    const int ty = ms_to_y(target);
    for (int x = kGx; x < kGx + kGw; x += 10) {
        cv.fill(x, ty, 6, 1, kTarget);
    }

    for (int i = 0; i < avail; ++i) {
        const float m = stats.sample(stats.count - avail + i);
        Color c = kGood;
        if (m > target * 1.5f) {
            c = kBad;
        } else if (m > target * 1.06f) {
            c = kWarn;
        }
        const int y = ms_to_y(m);
        const int x = kGx + kGw - (avail - i) * kBarW;
        cv.fill(x, y, kBarW, kGy + kGh - y, c);
    }

    // Axis label for the target line, right-aligned above it.
    snprintf(buf, sizeof(buf), "%4.1f", target);
    cv.text(kGx + kGw - 8 * (int)strlen(buf) - 4, ty - 11, 1, buf, kTarget);
}
