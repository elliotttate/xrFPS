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

    flipfps::Config config{};
    config.show_percentiles = true;

    std::vector<uint8_t> px(HUD_W * HUD_H * 4);
    hud_render(px.data(), stats, config, nullptr, /*bgra=*/true);

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

    const char* path = argc > 1 ? argv[1] : "hudtest.bmp";
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
