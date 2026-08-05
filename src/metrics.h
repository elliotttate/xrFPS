#pragma once

#include <windows.h>

#include <cstdint>

#include "stats.h"

namespace xrfps {

// System metrics the layer cannot gather from inside a game process (GPU
// utilization, VRAM, temperatures). The Control Center samples them with PDH
// and NVML and publishes them here; the layer reads whatever is fresh.
//
// Single writer, many readers, plain old data: a torn read costs one stale
// number for one HUD refresh, so no locking is warranted.
struct SharedMetrics {
    static constexpr uint32_t kMagic = 0x46465053; // 'FFPS'
    static constexpr const char* kName = "Local\\xrFPS_Metrics";

    uint32_t magic{kMagic};
    uint32_t version{2};

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

    float app_fps{-1.0f};
    float app_frame_ms{-1.0f};
    float app_cpu_frame_ms{-1.0f};
    float app_gpu_frame_ms{-1.0f};
    int32_t timing_source{};
    int32_t requested_timing_source{};
    char timing_source_name[32]{};

    bool fresh(uint64_t now_tick, uint64_t max_age_ms = 3000) const {
        return magic == kMagic && updated_tick != 0 && now_tick >= updated_tick &&
               (now_tick - updated_tick) <= max_age_ms;
    }
};

// The layer announces the VR application it is loaded into, so the Control
// Center never has to hunt for it by enumerating process modules.
struct SharedSession {
    static constexpr uint32_t kMagic = 0x46535353; // 'FSSS'
    static constexpr const char* kName = "Local\\xrFPS_Session";

    uint32_t magic{kMagic};
    uint32_t version{2};
    uint32_t pid{};
    uint64_t heartbeat_tick{};
    char app_name[64]{};
    float app_fps{-1.0f};
    float app_frame_ms{-1.0f};
    float app_cpu_frame_ms{-1.0f};
    float app_gpu_frame_ms{-1.0f};
    int32_t timing_source{};
    int32_t requested_timing_source{};
    char timing_source_name[32]{};

    bool fresh(uint64_t now_tick, uint64_t max_age_ms = 5000) const {
        return magic == kMagic && pid != 0 && heartbeat_tick != 0 && now_tick >= heartbeat_tick &&
               (now_tick - heartbeat_tick) <= max_age_ms;
    }
};

// Layer side: publish this process as the live VR app.
class SessionPublisher {
public:
    ~SessionPublisher() { stop(); }

    void start(const char* app_name) {
        if (view_ != nullptr) {
            return;
        }
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(SharedSession), SharedSession::kName);
        if (mapping_ == nullptr) {
            return;
        }
        view_ = (SharedSession*)MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSession));
        if (view_ == nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return;
        }
        view_->magic = SharedSession::kMagic;
        view_->version = 2;
        view_->pid = GetCurrentProcessId();
        strncpy_s(view_->app_name, sizeof(view_->app_name), app_name != nullptr ? app_name : "",
                  _TRUNCATE);
        view_->heartbeat_tick = GetTickCount64();
    }

    void publish_timing(TimingBackend source, int requested_source, const FrameStats& stats,
                        const char* source_name) {
        if (view_ == nullptr) {
            return;
        }
        view_->timing_source = (int32_t)source;
        view_->requested_timing_source = requested_source;
        view_->app_fps = stats.count > 0 ? stats.fps() : -1.0f;
        view_->app_frame_ms = stats.count > 0 ? stats.avg() : -1.0f;
        view_->app_cpu_frame_ms = stats.cpu_avg();
        view_->app_gpu_frame_ms = stats.gpu_avg();
        strncpy_s(view_->timing_source_name, sizeof(view_->timing_source_name),
                  source_name != nullptr ? source_name : "", _TRUNCATE);
    }

    // Cheap enough to call every frame; only stores once a second.
    void beat() {
        if (view_ == nullptr) {
            return;
        }
        const uint64_t now = GetTickCount64();
        if (now - view_->heartbeat_tick >= 1000) {
            view_->heartbeat_tick = now;
        }
    }

    void stop() {
        if (view_ != nullptr) {
            if (view_->pid == GetCurrentProcessId()) {
                view_->pid = 0; // readers stop attributing GPU load to us
            }
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

private:
    HANDLE mapping_{};
    SharedSession* view_{};
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
                if (view_ == nullptr) {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
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

} // namespace xrfps
