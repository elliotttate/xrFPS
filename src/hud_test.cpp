// Offline HUD render check: fills the stats ring with a synthetic frametime
// pattern and writes the rasterized HUD as a 32-bit BMP for visual inspection.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hud.h"

#pragma pack(push, 1)
struct BmpHeader {
    uint16_t magic{0x4D42};
    uint32_t file_size{};
    uint32_t reserved{};
    uint32_t data_offset{54};
    uint32_t info_size{40};
    int32_t width{};
    int32_t height{};
    uint16_t planes{1};
    uint16_t bpp{32};
    uint32_t compression{};
    uint32_t image_size{};
    int32_t ppm_x{2835};
    int32_t ppm_y{2835};
    uint32_t colors_used{};
    uint32_t colors_important{};
};
#pragma pack(pop)

int main(int argc, char** argv) {
    const bool dense = argc > 1 && std::strcmp(argv[1], "--dense") == 0;
    FrameStats stats{};
    stats.target_ms = 11.111f;

    // Mostly on-target with drift, occasional spikes, one rough patch.
    for (int i = 0; i < 400; ++i) {
        float m = 10.4f + 1.2f * std::sin((float)i * 0.09f);
        if (i % 47 == 0) {
            m += 9.0f;
        }
        if (i > 260 && i < 300) {
            m += 6.5f;
        }
        stats.push(m);
    }

    xrfps::Config config{};
    config.show_percentiles = true;
    xrfps::SharedMetrics metrics{};
    const xrfps::SharedMetrics* metric_source = nullptr;
    if (dense) {
        config.show_cpu_usage = true;
        config.show_gpu_usage = true;
        config.show_gpu_app_usage = true;
        config.show_vram = true;
        config.show_gpu_temp = true;
        config.show_cpu_temp = true;
        config.show_gpu_clock = true;
        config.show_ram = true;
        metrics.cpu_usage_pct = 48.0f;
        metrics.gpu_usage_pct = 91.0f;
        metrics.gpu_app_usage_pct = 76.0f;
        metrics.vram_used_mb = 8.4f * 1024.0f;
        metrics.gpu_temp_c = 79.0f;
        metrics.cpu_temp_c = 72.0f;
        metrics.gpu_clock_mhz = 2640.0f;
        metrics.ram_used_gb = 18.2f;
        metric_source = &metrics;
    }

    std::vector<uint8_t> px(HUD_W * HUD_H * 4);
    hud_render(px.data(), stats, config, metric_source, /*bgra=*/true);

    // Both horizontal graph borders should survive even in the dense mode.
    // This catches regressions where readout rows consume the graph entirely.
    int graph_border_rows = 0;
    for (int y = 0; y < HUD_H; ++y) {
        int border_pixels = 0;
        for (int x = 0; x < HUD_W; ++x) {
            const uint8_t* p = &px[(y * HUD_W + x) * 4];
            if (p[0] == 78 && p[1] == 65 && p[2] == 60) {
                ++border_pixels;
            }
        }
        if (border_pixels > HUD_W - 64) {
            ++graph_border_rows;
        }
    }
    if (graph_border_rows < 2) {
        std::fprintf(stderr, "frametime graph lost its minimum layout height\n");
        return 2;
    }

    // Composite onto a checkerboard so alpha is visible.
    std::vector<uint8_t> outpx(HUD_W * HUD_H * 4);
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint8_t* s = &px[(y * HUD_W + x) * 4];
            uint8_t* d = &outpx[(y * HUD_W + x) * 4];
            const uint8_t bg = (((x / 16) + (y / 16)) & 1) ? 90 : 60;
            const float a = s[3] / 255.0f;
            for (int c = 0; c < 3; ++c) {
                d[c] = (uint8_t)(s[c] * a + bg * (1.0f - a));
            }
            d[3] = 255;
        }
    }

    const char* path = dense ? (argc > 2 ? argv[2] : "hudtest-dense.bmp")
                             : (argc > 1 ? argv[1] : "hudtest.bmp");
    BmpHeader hdr{};
    hdr.width = HUD_W;
    hdr.height = -HUD_H; // top-down
    hdr.image_size = HUD_W * HUD_H * 4;
    hdr.file_size = sizeof(hdr) + hdr.image_size;

    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(outpx.data(), outpx.size(), 1, f);
    fclose(f);
    std::printf("wrote %s\n", path);
    return 0;
}
