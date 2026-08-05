#pragma once

#include "../src/metrics.h"

#include <string>

namespace xrfps {

// Samples system performance counters and publishes them for the layer.
// PDH supplies vendor-neutral CPU and per-process GPU utilization; NVML, when
// present, adds the VRAM/temperature/clock detail NVIDIA users expect.
class MetricsCollector {
public:
    ~MetricsCollector();

    bool start();          // creates the shared block; safe to call twice
    void stop();
    bool running() const { return view_ != nullptr; }

    // Call ~2x/second from the UI thread.
    void sample();

    const SharedMetrics& latest() const { return latest_; }
    const std::string& status() const { return status_; }

private:
    void detect_vr_app();

    SharedMetrics latest_{};
    std::string status_{"stopped"};

    void* mapping_{};
    SharedMetrics* view_{};

    // PDH
    void* pdh_query_{};
    void* counter_cpu_{};
    void* counter_gpu_{};
    bool pdh_primed_{false};

    // NVML (loaded dynamically; absent on non-NVIDIA systems)
    void* nvml_lib_{};
    void* nvml_device_{};

    unsigned long vr_pid_{};
    std::string vr_name_;
    void* session_mapping_{};
    const SharedSession* session_view_{};
};

} // namespace xrfps
