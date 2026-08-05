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
};
