#pragma once

#include <cstdint>
#include <string>

namespace xrfps {

struct RuntimeTimingSample {
    float frame_ms{-1.0f};
    float cpu_ms{-1.0f};
    float gpu_ms{-1.0f};
};

// Optional SteamVR compositor reader. openvr_api.dll is found and loaded at
// runtime; this class has no effect on systems using another OpenXR runtime.
class SteamVrTiming {
public:
    ~SteamVrTiming() { stop(); }

    SteamVrTiming() = default;
    SteamVrTiming(const SteamVrTiming&) = delete;
    SteamVrTiming& operator=(const SteamVrTiming&) = delete;

    // Returns newly observed application frames in chronological order.
    int collect(RuntimeTimingSample* output, int capacity);
    bool ready() const { return compositor_ != nullptr; }
    const std::string& status() const { return status_; }

    // Releases the OpenVR connection, but only the one this object opened.
    void stop();

private:
    bool start();

    void* module_{};
    void* compositor_{};
    // True only when openvr_api.dll was absent from the process until we loaded
    // it. If anything else in the process was already using OpenVR, the context
    // is not ours to shut down -- VR_ShutdownInternal is process-wide and would
    // disconnect them.
    bool owns_runtime_{false};
    uint32_t last_frame_index_{};
    double last_system_time_{};
    bool primed_{false};
    uint64_t next_retry_tick_{};
    std::string status_{"not initialized"};
};

} // namespace xrfps
