// Minimal OpenXR test app for exercising the FlipFPS layer without a full
// game: creates a session (D3D11 by default, D3D12 with --d3d12), suggests
// bindings and attaches an action set (so the layer's merge paths run), then
// clears each eye to an animated color in a standard frame loop. An optional
// sinusoidal CPU sleep gives the frametime graph something to show.
//
// Usage: flipfps_testapp [seconds] [--d3d12] [--no-wobble]
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_XR(expr)                                                  \
    do {                                                                \
        const XrResult check_result = (expr);                           \
        if (XR_FAILED(check_result)) {                                  \
            std::printf("FAILED %s -> %d\n", #expr, check_result);      \
            return 1;                                                   \
        }                                                               \
    } while (0)

namespace {

struct ViewTarget {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    int32_t width{};
    int32_t height{};
    std::vector<XrSwapchainImageD3D11KHR> images11;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> rtvs11;
    std::vector<XrSwapchainImageD3D12KHR> images12;
    ComPtr<ID3D12DescriptorHeap> rtv_heap12;
};

} // namespace

int main(int argc, char** argv) {
    int run_seconds = 120;
    bool wobble = true;
    bool use_d3d12 = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-wobble") == 0) {
            wobble = false;
        } else if (std::strcmp(argv[i], "--d3d12") == 0) {
            use_d3d12 = true;
        } else {
            run_seconds = std::atoi(argv[i]);
        }
    }

    // --- Instance ---
    std::vector<const char*> extensions{use_d3d12 ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME
                                                  : XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

    // Opt into Meta XR Operator's automation extension when a layer offers it,
    // so tests can drive controller poses programmatically.
    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> available(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data());
    for (const XrExtensionProperties& props : available) {
        if (std::strcmp(props.extensionName, "XR_METAX1_agentic_external_tool") == 0) {
            extensions.push_back("XR_METAX1_agentic_external_tool");
            std::printf("enabling XR_METAX1_agentic_external_tool\n");
        }
    }

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "FlipFPS Test App");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = (uint32_t)extensions.size();
    ici.enabledExtensionNames = extensions.data();

    XrInstance instance{XR_NULL_HANDLE};
    CHECK_XR(xrCreateInstance(&ici, &instance));

    XrInstanceProperties iprops{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &iprops);
    std::printf("runtime: %s, gfx: %s\n", iprops.runtimeName, use_d3d12 ? "d3d12" : "d3d11");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{XR_NULL_SYSTEM_ID};
    CHECK_XR(xrGetSystem(instance, &sgi, &system));

    // --- Graphics device ---
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> allocator12;
    ComPtr<ID3D12GraphicsCommandList> list12;
    ComPtr<ID3D12Fence> fence12;
    uint64_t fence_value12 = 0;
    HANDLE fence_event12 = nullptr;

    XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.systemId = system;

    if (use_d3d12) {
        PFN_xrGetD3D12GraphicsRequirementsKHR get_reqs = nullptr;
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                       (PFN_xrVoidFunction*)&get_reqs));
        XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        CHECK_XR(get_reqs(instance, system, &reqs));

        // Create the device on the adapter the runtime asked for (never the
        // default: on machines with virtual display adapters that picks the
        // wrong one and the runtime's resource sharing fails).
        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == reqs.adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == reqs.adapterLuid.HighPart) {
                std::wprintf(L"using adapter: %s\n", desc.Description);
                break;
            }
            adapter.Reset();
        }
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)))) {
            std::printf("D3D12CreateDevice failed\n");
            return 1;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12)))) {
            std::printf("CreateCommandQueue failed\n");
            return 1;
        }
        device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator12));
        device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator12.Get(), nullptr,
                                    IID_PPV_ARGS(&list12));
        list12->Close();
        device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12));
        fence_event12 = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        binding12.device = device12.Get();
        binding12.queue = queue12.Get();
        sci.next = &binding12;
    } else {
        PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                       (PFN_xrVoidFunction*)&get_reqs));
        XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK_XR(get_reqs(instance, system, &reqs));

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                     D3D11_SDK_VERSION, &device11, nullptr, &context11))) {
            std::printf("D3D11CreateDevice failed\n");
            return 1;
        }
        binding11.device = device11.Get();
        sci.next = &binding11;
    }

    XrSession session{XR_NULL_HANDLE};
    CHECK_XR(xrCreateSession(instance, &sci, &session));

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    XrSpace space{XR_NULL_HANDLE};
    CHECK_XR(xrCreateReferenceSpace(session, &rsci, &space));

    // --- App action set (exercises the layer's suggest/attach/sync merging) ---
    XrActionSet action_set{XR_NULL_HANDLE};
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    CHECK_XR(xrCreateActionSet(instance, &asci, &action_set));

    XrPath hands[2];
    xrStringToPath(instance, "/user/hand/left", &hands[0]);
    xrStringToPath(instance, "/user/hand/right", &hands[1]);

    XrAction trigger{XR_NULL_HANDLE};
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(aci.actionName, "fire");
    std::strcpy(aci.localizedActionName, "Fire");
    aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    CHECK_XR(xrCreateAction(action_set, &aci, &trigger));

    for (const char* profile :
         {"/interaction_profiles/khr/simple_controller", "/interaction_profiles/oculus/touch_controller"}) {
        XrPath profile_path, l, r;
        xrStringToPath(instance, profile, &profile_path);
        xrStringToPath(instance, "/user/hand/left/input/select/click", &l);
        xrStringToPath(instance, "/user/hand/right/input/select/click", &r);
        if (std::strstr(profile, "touch") != nullptr) {
            xrStringToPath(instance, "/user/hand/left/input/trigger/value", &l);
            xrStringToPath(instance, "/user/hand/right/input/trigger/value", &r);
        }
        XrActionSuggestedBinding suggested[] = {{trigger, l}, {trigger, r}};
        XrInteractionProfileSuggestedBinding spb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        spb.interactionProfile = profile_path;
        spb.suggestedBindings = suggested;
        spb.countSuggestedBindings = 2;
        const XrResult r2 = xrSuggestInteractionProfileBindings(instance, &spb);
        std::printf("suggest %s -> %d\n", profile, r2);
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set;
    CHECK_XR(xrAttachSessionActionSets(session, &attach));
    std::printf("action sets attached\n");

    // --- View swapchains ---
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                      &view_count, nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      view_count, &view_count, config_views.data());

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    int64_t color_format = formats.empty() ? 28 : formats[0];
    for (const int64_t f : formats) {
        if (f == 29 || f == 91) { // prefer an SRGB format
            color_format = f;
            break;
        }
    }

    const UINT rtv_stride =
        use_d3d12 ? device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;

    std::vector<ViewTarget> targets(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = color_format;
        sc.sampleCount = 1;
        sc.width = config_views[i].recommendedImageRectWidth;
        sc.height = config_views[i].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        CHECK_XR(xrCreateSwapchain(session, &sc, &targets[i].swapchain));
        targets[i].width = sc.width;
        targets[i].height = sc.height;

        uint32_t image_count = 0;
        xrEnumerateSwapchainImages(targets[i].swapchain, 0, &image_count, nullptr);
        if (use_d3d12) {
            targets[i].images12.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(targets[i].swapchain, image_count, &image_count,
                                       (XrSwapchainImageBaseHeader*)targets[i].images12.data());

            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = image_count;
            device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&targets[i].rtv_heap12));
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                targets[i].rtv_heap12->GetCPUDescriptorHandleForHeapStart();
            for (uint32_t j = 0; j < image_count; ++j) {
                device12->CreateRenderTargetView(targets[i].images12[j].texture, nullptr, handle);
                handle.ptr += rtv_stride;
            }
        } else {
            targets[i].images11.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            xrEnumerateSwapchainImages(targets[i].swapchain, image_count, &image_count,
                                       (XrSwapchainImageBaseHeader*)targets[i].images11.data());
        }
    }
    std::printf("%u views, %dx%d, format %lld\n", view_count, targets[0].width, targets[0].height,
                (long long)color_format);

    // --- Frame loop ---
    bool session_running = false;
    bool quit = false;
    const auto start = std::chrono::steady_clock::now();
    uint64_t frame_index = 0;

    while (!quit) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(run_seconds)) {
            quit = true;
        }

        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* state_event = (const XrEventDataSessionStateChanged*)&event;
                std::printf("session state -> %d\n", state_event->state);
                if (state_event->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xrBeginSession(session, &begin);
                    session_running = true;
                } else if (state_event->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    session_running = false;
                } else if (state_event->state == XR_SESSION_STATE_EXITING) {
                    quit = true;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!session_running) {
            Sleep(10);
            continue;
        }

        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session, &wait_info, &frame_state))) {
            break;
        }
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &begin_info);

        XrActiveActionSet active{action_set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        xrSyncActions(session, &sync);

        // Synthetic CPU load so the frametime graph has structure.
        if (wobble) {
            const double t = (double)frame_index / 90.0;
            Sleep((DWORD)(3.0 + 2.5 * std::sin(t)));
        }

        std::vector<XrCompositionLayerProjectionView> proj_views;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerBaseHeader*> layers;

        if (frame_state.shouldRender) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = frame_state.predictedDisplayTime;
            vli.space = space;
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::vector<XrView> views(view_count, {XR_TYPE_VIEW});
            uint32_t located = 0;
            xrLocateViews(session, &vli, &view_state, view_count, &located, views.data());

            const float phase = (float)frame_index * 0.01f;
            const float color[4] = {0.30f + 0.15f * std::sin(phase),
                                    0.45f + 0.15f * std::sin(phase * 1.3f + 2.0f),
                                    0.65f + 0.15f * std::sin(phase * 0.7f + 4.0f), 1.0f};

            proj_views.resize(view_count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            for (uint32_t i = 0; i < view_count; ++i) {
                ViewTarget& target = targets[i];
                uint32_t image_index = 0;
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(target.swapchain, &acquire, &image_index);
                XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                image_wait.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(target.swapchain, &image_wait);

                if (use_d3d12) {
                    allocator12->Reset();
                    list12->Reset(allocator12.Get(), nullptr);

                    ID3D12Resource* tex = target.images12[image_index].texture;
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = tex;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    list12->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                        target.rtv_heap12->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += (SIZE_T)image_index * rtv_stride;
                    list12->ClearRenderTargetView(rtv, color, 0, nullptr);

                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                    list12->ResourceBarrier(1, &barrier);
                    list12->Close();

                    ID3D12CommandList* lists[] = {list12.Get()};
                    queue12->ExecuteCommandLists(1, lists);
                    queue12->Signal(fence12.Get(), ++fence_value12);
                    if (fence12->GetCompletedValue() < fence_value12) {
                        fence12->SetEventOnCompletion(fence_value12, fence_event12);
                        WaitForSingleObject(fence_event12, 100);
                    }
                } else {
                    ID3D11Texture2D* tex = target.images11[image_index].texture;
                    auto& rtv = target.rtvs11[tex];
                    if (!rtv) {
                        device11->CreateRenderTargetView(tex, nullptr, &rtv);
                    }
                    context11->ClearRenderTargetView(rtv.Get(), color);
                }

                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(target.swapchain, &release);

                proj_views[i].pose = views[i].pose;
                proj_views[i].fov = views[i].fov;
                proj_views[i].subImage.swapchain = target.swapchain;
                proj_views[i].subImage.imageRect = {{0, 0}, {target.width, target.height}};
            }
            projection.space = space;
            projection.viewCount = view_count;
            projection.views = proj_views.data();
            layers.push_back((XrCompositionLayerBaseHeader*)&projection);
        }

        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = frame_state.predictedDisplayTime;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount = (uint32_t)layers.size();
        end_info.layers = layers.data();
        xrEndFrame(session, &end_info);

        ++frame_index;
        if (frame_index % 300 == 0) {
            std::printf("frame %llu\n", (unsigned long long)frame_index);
        }
    }

    std::printf("shutting down after %llu frames\n", (unsigned long long)frame_index);
    if (fence_event12 != nullptr) {
        CloseHandle(fence_event12);
    }
    for (ViewTarget& target : targets) {
        if (target.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(target.swapchain);
        }
    }
    xrDestroySpace(space);
    xrDestroySession(session);
    xrDestroyInstance(instance);
    return 0;
}
