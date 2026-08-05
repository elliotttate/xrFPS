#pragma once

#include <algorithm>
#include <cstdint>

enum class TimingBackend : int32_t {
    OpenXR = 0,
    SteamVR = 1,
    Meta = 2,
};

// A synchronized history of application frame intervals and, when the active
// runtime exposes them, CPU/GPU frametimes for those frames.
struct FrameStats {
    static constexpr int N = 512;
    static constexpr float kRateWindowMs = 1000.0f;

    float ms[N]{};
    float cpu_ms[N]{};
    float gpu_ms[N]{};
    int head{0};
    int count{0};
    float target_ms{11.111f}; // refreshed from xrWaitFrame's predictedDisplayPeriod
    TimingBackend source{TimingBackend::OpenXR};

    void clear(TimingBackend new_source) {
        head = 0;
        count = 0;
        source = new_source;
        std::fill_n(ms, N, 0.0f);
        std::fill_n(cpu_ms, N, -1.0f);
        std::fill_n(gpu_ms, N, -1.0f);
    }

    void push(float frame_ms, float cpu = -1.0f, float gpu = -1.0f) {
        if (frame_ms <= 0.0f || frame_ms > 60000.0f) {
            return;
        }
        ms[head] = frame_ms;
        cpu_ms[head] = cpu > 0.0f ? cpu : -1.0f;
        gpu_ms[head] = gpu > 0.0f ? gpu : -1.0f;
        head = (head + 1) % N;
        if (count < N) {
            ++count;
        }
    }

    // i = 0 is the oldest retained sample.
    int slot(int i) const { return (head - count + i + 2 * N) % N; }
    float sample(int i) const { return ms[slot(i)]; }
    float cpu_sample(int i) const { return cpu_ms[slot(i)]; }
    float gpu_sample(int i) const { return gpu_ms[slot(i)]; }

    // Number of newest frames covering approximately one second. A time-based
    // window behaves consistently at 45, 80, 90, 120 and 144 Hz.
    int rate_sample_count() const {
        float elapsed = 0.0f;
        int samples = 0;
        for (int i = count - 1; i >= 0; --i) {
            elapsed += sample(i);
            ++samples;
            if (elapsed >= kRateWindowMs) {
                break;
            }
        }
        return samples;
    }

    float avg(int last_n = 0) const {
        if (count == 0) {
            return 0.0f;
        }
        if (last_n <= 0) {
            last_n = rate_sample_count();
        }
        last_n = std::min(last_n, count);
        float sum = 0.0f;
        for (int i = count - last_n; i < count; ++i) {
            sum += sample(i);
        }
        return last_n > 0 ? sum / (float)last_n : 0.0f;
    }

    float fps() const {
        const int samples = rate_sample_count();
        if (samples == 0) {
            return 0.0f;
        }
        float elapsed = 0.0f;
        for (int i = count - samples; i < count; ++i) {
            elapsed += sample(i);
        }
        return elapsed > 0.0f ? 1000.0f * (float)samples / elapsed : 0.0f;
    }

    float auxiliary_avg(bool cpu) const {
        const int samples = rate_sample_count();
        float sum = 0.0f;
        int valid = 0;
        for (int i = count - samples; i < count; ++i) {
            const float value = cpu ? cpu_sample(i) : gpu_sample(i);
            if (value > 0.0f) {
                sum += value;
                ++valid;
            }
        }
        return valid > 0 ? sum / (float)valid : -1.0f;
    }

    float cpu_avg() const { return auxiliary_avg(true); }
    float gpu_avg() const { return auxiliary_avg(false); }
    bool has_cpu() const { return cpu_avg() > 0.0f; }
    bool has_gpu() const { return gpu_avg() > 0.0f; }

    // Worst frametime at the given percentile of the retained window, e.g.
    // 0.99 for the classic "1% low".
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
        idx = std::clamp(idx, 0, count - 1);
        return sorted[idx];
    }

    float percentile_fps(float p) const {
        const float value = percentile_ms(p);
        return value > 0.0f ? 1000.0f / value : 0.0f;
    }
};
