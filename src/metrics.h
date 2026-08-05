#pragma once

#include <windows.h>

#include <cstdint>

namespace flipfps {

// System metrics the layer cannot gather from inside a game process (GPU
// utilization, VRAM, temperatures). The Control Center samples them with PDH
// and NVML and publishes them here; the layer reads whatever is fresh.
//
// Single writer, many readers, plain old data: a torn read costs one stale
// number for one HUD refresh, so no locking is warranted.
struct SharedMetrics {
    static constexpr uint32_t kMagic = 0x46465053; // 'FFPS'
    static constexpr const char* kName = "Local\\FlipFPS_Metrics";

    uint32_t magic{kMagic};
    uint32_t version{1};

    // Milliseconds since the writer's boot-relative clock; readers compare
    // against their own GetTickCount64() to detect a stopped collector.
    uint64_t updated_tick{};

    float cpu_usage_pct{-1.0f};       // whole system
    float gpu_usage_pct{-1.0f};       // whole GPU (busiest 3D engine)
    float gpu_app_usage_pct{-1.0f};   // the focused VR app's share
    float vram_used_mb{-1.0f};
    float vram_total_mb{-1.0f};
    float gpu_temp_c{-1.0f};
    float cpu_temp_c{-1.0f};
    float gpu_clock_mhz{-1.0f};
    float ram_used_gb{-1.0f};
    float ram_total_gb{-1.0f};

    uint32_t app_pid{};
    char app_name[64]{};

    bool fresh(uint64_t now_tick, uint64_t max_age_ms = 3000) const {
        return magic == kMagic && updated_tick != 0 && now_tick >= updated_tick &&
               (now_tick - updated_tick) <= max_age_ms;
    }
};

// Reader side: maps the block if the collector is running. Safe to call
// repeatedly; connects as soon as the Control Center starts publishing.
class MetricsReader {
public:
    ~MetricsReader() { close(); }

    const SharedMetrics* get() {
        if (view_ == nullptr) {
            mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, SharedMetrics::kName);
            if (mapping_ != nullptr) {
                view_ = (const SharedMetrics*)MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0,
                                                            sizeof(SharedMetrics));
            }
        }
        if (view_ != nullptr && view_->fresh(GetTickCount64())) {
            return view_;
        }
        return nullptr;
    }

    void close() {
        if (view_ != nullptr) {
            UnmapViewOfFile((const void*)view_);
            view_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

private:
    HANDLE mapping_{};
    const SharedMetrics* view_{};
};

} // namespace flipfps
