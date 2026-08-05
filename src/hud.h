#pragma once

#include <cstdint>

#include "config.h"
#include "metrics.h"
#include "stats.h"

constexpr int HUD_W = 512;
constexpr int HUD_H = 256;

// Software-rasterizes the HUD into `out`, which must hold HUD_W * HUD_H * 4
// bytes. Straight (unpremultiplied) alpha; `bgra` selects byte order to match
// the swapchain format. `metrics` may be null when the collector isn't
// running — those rows are then skipped.
void hud_render(uint8_t* out, const FrameStats& stats, const flipfps::Config& config,
                const flipfps::SharedMetrics* metrics, bool bgra);
