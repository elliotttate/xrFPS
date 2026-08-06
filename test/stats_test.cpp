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

    // "% on target" must read the same one-second window as fps()/avg(). A
    // full-history reading would still show old stutter long after the frame
    // rate readout above it had recovered.
    FrameStats recovered{};
    const float target = 1000.0f / 90.0f;
    for (int i = 0; i < 200; ++i) {
        recovered.push(target * 2.0f); // an earlier, badly missed stretch
    }
    for (int i = 0; i < 200; ++i) {
        recovered.push(target); // ... followed by a clean recent second
    }
    ok &= close_to(recovered.on_target_pct(target * 1.06f), 100.0f, 0.01f,
                   "on-target ignores frames older than the rate window");

    FrameStats missing{};
    for (int i = 0; i < 200; ++i) {
        missing.push(target * 1.5f);
    }
    ok &= close_to(missing.on_target_pct(target * 1.06f), 0.0f, 0.01f, "on-target detects misses");

    FrameStats empty{};
    ok &= empty.on_target_pct(target) < 0.0f; // nothing to report yet

    // percentile_ms ranks ascending by nearest rank, so 0.99 is the slow tail
    // that the "1% low" readout reports. With 5 slow frames in 100, the 99th
    // percentile sits inside that tail; the median must not.
    FrameStats spiky{};
    for (int i = 0; i < 100; ++i) {
        spiky.push(i < 95 ? 10.0f : 50.0f);
    }
    ok &= close_to(spiky.percentile_ms(0.99f), 50.0f, 0.01f, "1% low reports the slow tail");
    ok &= close_to(spiky.percentile_ms(0.5f), 10.0f, 0.01f, "median ignores the slow tail");
    ok &= close_to(spiky.percentile_ms(0.0f), 10.0f, 0.01f, "0th percentile is the fastest frame");

    fast.clear(TimingBackend::Meta);
    ok &= fast.count == 0 && fast.source == TimingBackend::Meta;
    return ok ? 0 : 1;
}
