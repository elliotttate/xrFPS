#include "../src/timing.h"

#include <windows.h>

#include <cstdio>

int main() {
    xrfps::SteamVrTiming timing;
    for (int attempt = 0; attempt < 100; ++attempt) {
        xrfps::RuntimeTimingSample samples[128]{};
        const int count = timing.collect(samples, 128);
        if (count > 0) {
            const auto& latest = samples[count - 1];
            std::printf("source=%s samples=%d frame=%.3fms fps=%.2f cpu=%.3fms gpu=%.3fms\n",
                        timing.status().c_str(), count, latest.frame_ms,
                        1000.0f / latest.frame_ms, latest.cpu_ms, latest.gpu_ms);
            return 0;
        }
        Sleep(30);
    }
    std::fprintf(stderr, "source=%s ready=%d no application samples received\n",
                 timing.status().c_str(), timing.ready() ? 1 : 0);
    return timing.ready() ? 2 : 1;
}
