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

#include "stats.h"

namespace flipfps {

// Downchain OpenXR entry points the layer needs, resolved once per instance.
struct NextFns {
    PFN_xrGetInstanceProcAddr getInstanceProcAddr{};
    PFN_xrDestroyInstance destroyInstance{};
    PFN_xrCreateSession createSession{};
    PFN_xrDestroySession destroySession{};
    PFN_xrSuggestInteractionProfileBindings suggestBindings{};
    PFN_xrAttachSessionActionSets attachActionSets{};
    PFN_xrSyncActions syncActions{};
    PFN_xrWaitFrame waitFrame{};
    PFN_xrEndFrame endFrame{};
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
};

enum class Gfx { None, D3D11, D3D12 };

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
    std::vector<XrSwapchainImageD3D11KHR> images_d3d11{};
    std::vector<XrSwapchainImageD3D12KHR> images_d3d12{};

    FrameStats stats{};
    double last_end_frame_qpc_ms{0.0};

    // Gesture state.
    bool visible{false};
    bool force_show{false};
    bool f10_was_down{false};
    double flip_since_ms{0.0};
    double unflip_since_ms{0.0};
    XrPosef anchor_pose{{0, 0, 0, 1}, {0, 0, 0}};
    bool anchor_valid{false};

    std::vector<uint8_t> pixels{};
    std::vector<const XrCompositionLayerBaseHeader*> layer_scratch{};
    XrCompositionLayerQuad quad{};
};

Session* session_create(XrInstance instance, XrSession session, const XrSessionCreateInfo* create_info, const NextFns* fns);
void session_destroy(Session* s);
void session_note_display_period(Session* s, XrDuration period_ns);

// Runs stats + gesture + overlay injection, then calls the downchain xrEndFrame.
XrResult session_end_frame(Session* s, const XrFrameEndInfo* frame_end_info);

} // namespace flipfps
