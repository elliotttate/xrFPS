#pragma once

#include <cstdint>

#include "stats.h"

constexpr int HUD_W = 512;
constexpr int HUD_H = 256;

// Software-rasterizes the HUD (FPS readout + frametime graph) into `out`,
// which must hold HUD_W * HUD_H * 4 bytes. Straight (unpremultiplied) alpha.
// `bgra` selects byte order to match the swapchain format.
void hud_render(uint8_t* out, const FrameStats& stats, bool bgra);
