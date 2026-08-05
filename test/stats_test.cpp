#include "../src/stats.h"

#include <cmath>
#include <cstdio>

namespace {

bool close_to(float actual, float expected, float tolerance, const char* label) {
    if (std::fabs(actual - expected) <= tolerance) {
        return true;
    }
    std::fprintf(stderr, "%s: got %.3f, expected %.3f (+/- %.3f)\n", label, actual,
                 expected, tolerance);
    return false;
}

} // namespace

int main() {
    bool ok = true;

    FrameStats fast{};
    for (int i = 0; i < 300; ++i) {
        fast.push(1000.0f / 144.0f, 3.5f, 5.75f);
    }
    ok &= close_to(fast.fps(), 144.0f, 0.05f, "144 Hz rate");
    ok &= close_to(fast.avg(), 1000.0f / 144.0f, 0.01f, "144 Hz frametime");
    ok &= close_to(fast.cpu_avg(), 3.5f, 0.01f, "CPU average");
    ok &= close_to(fast.gpu_avg(), 5.75f, 0.01f, "GPU average");

    FrameStats slow{};
    for (int i = 0; i < 200; ++i) {
        slow.push(12.5f);
    }
    ok &= close_to(slow.fps(), 80.0f, 0.05f, "80 Hz rate");

    FrameStats hitch{};
    hitch.push(2000.0f);
    ok &= close_to(hitch.fps(), 0.5f, 0.01f, "long frame is retained");

    fast.clear(TimingBackend::Meta);
    ok &= fast.count == 0 && fast.source == TimingBackend::Meta;
    return ok ? 0 : 1;
}
