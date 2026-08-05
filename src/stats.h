#pragma once

// Ring buffer of recent frame intervals, pushed once per xrEndFrame.
struct FrameStats {
    static constexpr int N = 512;

    float ms[N]{};
    int head{0};
    int count{0};
    float target_ms{11.111f}; // refreshed from xrWaitFrame's predictedDisplayPeriod

    void push(float m) {
        if (m <= 0.0f || m > 1000.0f) {
            return; // ignore pauses/hitches from load screens or debugger stalls
        }
        ms[head] = m;
        head = (head + 1) % N;
        if (count < N) {
            ++count;
        }
    }

    // i = 0 is the oldest retained sample.
    float sample(int i) const {
        return ms[(head - count + i + 2 * N) % N];
    }

    float avg(int last_n = 90) const {
        if (count == 0) {
            return 0.0f;
        }
        if (last_n > count) {
            last_n = count;
        }
        float sum = 0.0f;
        for (int i = count - last_n; i < count; ++i) {
            sum += sample(i);
        }
        return sum / (float)last_n;
    }

    float fps() const {
        const float a = avg();
        return a > 0.0f ? 1000.0f / a : 0.0f;
    }

    // Worst frametime at the given percentile of the retained window, e.g.
    // 0.99 for the classic "1% low". Insertion sort over <=512 mostly-sorted
    // samples is well under the cost of the HUD raster it feeds.
    float percentile_ms(float p) const {
        if (count == 0) {
            return 0.0f;
        }
        float sorted[N];
        for (int i = 0; i < count; ++i) {
            sorted[i] = sample(i);
        }
        for (int i = 1; i < count; ++i) {
            const float key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) {
                sorted[j + 1] = sorted[j];
                --j;
            }
            sorted[j + 1] = key;
        }
        int idx = (int)(p * (float)(count - 1) + 0.5f);
        if (idx < 0) {
            idx = 0;
        }
        if (idx >= count) {
            idx = count - 1;
        }
        return sorted[idx];
    }

    // FPS equivalent of a percentile frametime (higher percentile = worse).
    float percentile_fps(float p) const {
        const float ms = percentile_ms(p);
        return ms > 0.0f ? 1000.0f / ms : 0.0f;
    }
};
