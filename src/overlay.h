#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

#include "config.h"
#include "metrics.h"
#include "stats.h"
#include "timing.h"

namespace xrfps {

// Downchain OpenXR entry points the layer needs, resolved once per instance.
struct NextFns {
    PFN_xrGetInstanceProcAddr getInstanceProcAddr{};
    PFN_xrGetInstanceProperties getInstanceProperties{};
    PFN_xrDestroyInstance destroyInstance{};
    PFN_xrCreateSession createSession{};
    PFN_xrDestroySession destroySession{};
    PFN_xrSuggestInteractionProfileBindings suggestBindings{};
    PFN_xrAttachSessionActionSets attachActionSets{};
    PFN_xrSyncActions syncActions{};
    PFN_xrWaitFrame waitFrame{};
    PFN_xrBeginFrame beginFrame{};
    PFN_xrEndFrame endFrame{};
    PFN_xrLocateViews locateViews{};
    PFN_xrPollEvent pollEvent{};
    PFN_xrStringToPath stringToPath{};
    PFN_xrCreateActionSet createActionSet{};
    PFN_xrDestroyActionSet destroyActionSet{};
    PFN_xrCreateAction createAction{};
    PFN_xrCreateActionSpace createActionSpace{};
    PFN_xrCreateReferenceSpace createReferenceSpace{};
    PFN_xrDestroySpace destroySpace{};
    PFN_xrLocateSpace locateSpace{};
    PFN_xrGetSystemProperties getSystemProperties{};
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats{};
    PFN_xrCreateSwapchain createSwapchain{};
    PFN_xrDestroySwapchain destroySwapchain{};
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages{};
    PFN_xrAcquireSwapchainImage acquireSwapchainImage{};
    PFN_xrWaitSwapchainImage waitSwapchainImage{};
    PFN_xrReleaseSwapchainImage releaseSwapchainImage{};
    PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumeratePerformanceMetricsCounterPathsMETA{};
    PFN_xrSetPerformanceMetricsStateMETA setPerformanceMetricsStateMETA{};
    PFN_xrQueryPerformanceMetricsCounterMETA queryPerformanceMetricsCounterMETA{};
};

enum class Gfx { None, D3D11, D3D12 };

struct SessionTiming;  // frame_timing.cpp

struct Session {
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    const NextFns* fns{};
    XrSession session{XR_NULL_HANDLE};

    Gfx gfx{Gfx::None};
    ComPtr<ID3D11Device> d11_device{};
    ComPtr<ID3D11DeviceContext> d11_context{};
    ComPtr<ID3D12Device> d12_device{};
    ComPtr<ID3D12CommandQueue> d12_queue{};

    // D3D12 upload machinery (created with the swapchain).
    ComPtr<ID3D12CommandAllocator> d12_allocator{};
    ComPtr<ID3D12GraphicsCommandList> d12_list{};
    ComPtr<ID3D12Fence> d12_fence{};
    ComPtr<ID3D12Resource> d12_upload{};
    uint8_t* d12_upload_ptr{};
    uint64_t d12_fence_value{};

    XrSpace ref_space{XR_NULL_HANDLE};  // LOCAL
    XrSpace view_space{XR_NULL_HANDLE}; // VIEW
    XrSpace grip_space[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool our_actionset_attached{false};

    uint32_t max_layers{16};

    XrSwapchain swapchain{XR_NULL_HANDLE};
    int64_t swapchain_format{0};
    bool swapchain_bgra{false};
    bool ever_uploaded{false};
    int swapchain_attempts{0};
    std::vector<XrSwapchainImageD3D11KHR> images_d3d11{};
    std::vector<XrSwapchainImageD3D12KHR> images_d3d12{};

    FrameStats stats{};
    double last_successful_end_frame_qpc_ms{0.0};
    double begin_frame_qpc_ms{0.0};
    double last_upload_qpc_ms{0.0};
    // A frame that repaints the HUD also pays for our raster, swapchain
    // acquire/wait/upload and release, all before xrEndFrame returns. Counting
    // that interval would report the tool's own cost as the application's, as a
    // periodic spike at exactly the refresh rate.
    bool hud_refreshed_this_frame{false};

    // Runtime timing selection. SteamVR provides compositor-owned frame
    // records; Meta provides runtime CPU/GPU counters; OpenXR is the universal
    // successful-submission fallback.
    SteamVrTiming steamvr_timing{};
    bool runtime_is_steamvr{false};
    bool meta_timing_available{false};
    XrPath meta_cpu_path{XR_NULL_PATH};
    XrPath meta_gpu_path{XR_NULL_PATH};
    float latest_meta_cpu_ms{-1.0f};
    float latest_meta_gpu_ms{-1.0f};
    double last_timing_poll_qpc_ms{0.0};
    int last_timing_source_config{-1};

    // Recent xrWaitFrame display periods. Runtimes interleave the true panel
    // period with longer half-rate pacing hints (SteamVR motion smoothing,
    // VDXR SSW), so the target is drawn from the SHORT end of this window --
    // taking the max would report half the headset's real refresh rate.
    static constexpr int kPeriodWindow = 90;
    float period_window_ms[kPeriodWindow]{};
    int period_index{0};
    int period_count{0};

    // Gesture state.
    bool visible{false};
    bool force_show{false};
    bool f10_was_down{false};
    double flip_since_ms{0.0};
    double unflip_since_ms{0.0};
    int anchor_hand{-1};
    XrPosef anchor_pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrSpace anchor_space{XR_NULL_HANDLE};
    bool anchor_valid{false};
    bool controller_available{false};
    bool controller_availability_known{false};
    bool fallback_active{false};

    // Floating placements use a softly lagging headset pose so
    // the panel follows the user without being glued to every head tremor.
    bool follow_valid{false};
    XrVector3f follow_position{0, 0, 0};
    float follow_yaw_rad{0.0f};
    double last_follow_update_ms{0.0};

    // Test pose injection (XRFPS_TEST_POSE=1): a shared-memory grip pose
    // replaces the left controller for automated testing on runtimes without
    // drivable tracking. Layout: float[8] = {active, px, py, pz, qx, qy, qz, qw}.
    bool test_pose_env{false};
    bool test_pose_env_checked{false};
    HANDLE test_pose_mapping{};
    const volatile float* test_pose_view{};

    // Live settings, re-read when the Control Center rewrites the file.
    Config config{};
    unsigned long long config_stamp{};
    double last_config_check_ms{0.0};
    MetricsReader metrics{};
    SessionPublisher session_info{};

    // Frame timeline: GPU timers for this session, and when xrEndFrame handed
    // the frame to the runtime (after xrFPS's own HUD work).
    SessionTiming* timing{};
    uint64_t submit_qpc{0};

    std::vector<uint8_t> pixels{};
    std::vector<const XrCompositionLayerBaseHeader*> layer_scratch{};
    XrCompositionLayerQuad quad{};
};

Session* session_create(XrInstance instance, XrSession session, const XrSessionCreateInfo* create_info,
                        const NextFns* fns, const char* app_name, const char* runtime_name);
void session_destroy(Session* s);
void session_note_display_period(Session* s, XrDuration period_ns);
void session_begin_frame(Session* s);

// Runs stats + gesture + overlay injection, then calls the downchain xrEndFrame.
XrResult session_end_frame(Session* s, const XrFrameEndInfo* frame_end_info);

} // namespace xrfps
