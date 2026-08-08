#pragma once
#include <cstdint>

namespace xrfps {

struct CaptureHeader {
    static constexpr const char* kName = "Local\\xrFPS_CaptureFrame";
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t sequence{0};
    uint32_t active{0};
};

constexpr uint32_t kMaxCaptureWidth = 2048;
constexpr uint32_t kMaxCaptureHeight = 2048;
constexpr uint32_t kCaptureMemorySize = sizeof(CaptureHeader) + (kMaxCaptureWidth * kMaxCaptureHeight * 4);

} // namespace xrfps
