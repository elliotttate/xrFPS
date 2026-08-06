#include <windows.h>

#include <cstdio>

#include "../src/config.h"

int main() {
    char temp_dir[MAX_PATH]{};
    char temp_file[MAX_PATH]{};
    if (GetTempPathA(MAX_PATH, temp_dir) == 0 ||
        GetTempFileNameA(temp_dir, "xrf", 0, temp_file) == 0) {
        std::fprintf(stderr, "could not create temporary config path\n");
        return 1;
    }

    xrfps::Config written{};
    written.controller_placement = 8;
    written.fallback_placement = 6;
    written.panel_roll_deg = 17.0f;
    const bool saved = written.save(temp_file);

    xrfps::Config loaded{};
    const bool read = loaded.load(temp_file);
    const bool matches = loaded.controller_placement == 8 && loaded.fallback_placement == 6 &&
                         loaded.panel_roll_deg == 17.0f;

    DeleteFileA(temp_file);
    const std::string temp_sidecar = std::string{temp_file} + ".tmp";
    DeleteFileA(temp_sidecar.c_str());

    if (!saved || !read || !matches) {
        std::fprintf(stderr,
                     "placement config round trip failed: saved=%d read=%d controller=%d fallback=%d roll=%.1f\n",
                     (int)saved, (int)read, loaded.controller_placement,
                     loaded.fallback_placement, loaded.panel_roll_deg);
        return 1;
    }
    return 0;
}
