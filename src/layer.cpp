// xrFPS — implicit OpenXR API layer. Turn a controller upside down to see
// an FPS/frametime HUD composited over any OpenXR application.
//
// Mechanism: the layer appends its own action set (one grip-pose action) to the
// app's suggested bindings / attached action sets / per-frame sync, watches the
// grip orientation at xrEndFrame time, and when flipped injects one extra
// XrCompositionLayerQuad rendered by a tiny CPU rasterizer.
#include "overlay.h"

#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame_timing.h"
#include "log.h"

namespace {

struct Global {
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrGetInstanceProcAddr next_gipa{};
    xrfps::NextFns fns{};

    char app_name[64]{};
    char runtime_name[XR_MAX_RUNTIME_NAME_SIZE]{};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction grip_action{XR_NULL_HANDLE};
    XrPath hand_path[2]{XR_NULL_PATH, XR_NULL_PATH};
    XrPath grip_binding[2]{XR_NULL_PATH, XR_NULL_PATH};

    std::mutex mtx;
    std::unordered_map<XrSession, xrfps::Session*> sessions;
};

Global g;

xrfps::Session* find_session(XrSession session) {
    std::lock_guard<std::mutex> lock(g.mtx);
    const auto it = g.sessions.find(session);
    return it != g.sessions.end() ? it->second : nullptr;
}

template <typename T>
void load_next(const char* name, T& out) {
    g.next_gipa(g.instance, name, (PFN_xrVoidFunction*)&out);
}

void create_our_actions() {
    g.fns.stringToPath(g.instance, "/user/hand/left", &g.hand_path[0]);
    g.fns.stringToPath(g.instance, "/user/hand/right", &g.hand_path[1]);
    g.fns.stringToPath(g.instance, "/user/hand/left/input/grip/pose", &g.grip_binding[0]);
    g.fns.stringToPath(g.instance, "/user/hand/right/input/grip/pose", &g.grip_binding[1]);

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "xrfps");
    strcpy_s(set_info.localizedActionSetName, "xrFPS");
    if (XR_FAILED(g.fns.createActionSet(g.instance, &set_info, &g.action_set))) {
        flog("xrCreateActionSet failed");
        return;
    }

    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(action_info.actionName, "grip_pose");
    strcpy_s(action_info.localizedActionName, "Grip pose");
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    action_info.countSubactionPaths = 2;
    action_info.subactionPaths = g.hand_path;
    if (XR_FAILED(g.fns.createAction(g.action_set, &action_info, &g.grip_action))) {
        flog("xrCreateAction failed");
        g.grip_action = XR_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL hook_xrCreateSession(XrInstance instance,
                                                    const XrSessionCreateInfo* create_info,
                                                    XrSession* session) {
    const XrResult r = g.fns.createSession(instance, create_info, session);
    if (XR_SUCCEEDED(r)) {
        auto* s = xrfps::session_create(instance, *session, create_info, &g.fns, g.app_name,
                                        g.runtime_name);
        xrfps::frame_timing::session_created(s, instance, create_info);
        std::lock_guard<std::mutex> lock(g.mtx);
        g.sessions[*session] = s;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrDestroySession(XrSession session) {
    xrfps::Session* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        const auto it = g.sessions.find(session);
        if (it != g.sessions.end()) {
            s = it->second;
            g.sessions.erase(it);
        }
    }
    xrfps::frame_timing::session_destroyed(s);
    xrfps::session_destroy(s);
    return g.fns.destroySession(session);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
    if (g.grip_action == XR_NULL_HANDLE) {
        return g.fns.suggestBindings(instance, suggested);
    }

    std::vector<XrActionSuggestedBinding> bindings(
        suggested->suggestedBindings, suggested->suggestedBindings + suggested->countSuggestedBindings);
    bindings.push_back({g.grip_action, g.grip_binding[0]});
    bindings.push_back({g.grip_action, g.grip_binding[1]});

    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.suggestedBindings = bindings.data();
    merged.countSuggestedBindings = (uint32_t)bindings.size();

    const XrResult r = g.fns.suggestBindings(instance, &merged);
    if (XR_FAILED(r)) {
        // Profile without grip poses (eye gaze, trackers, ...): step aside.
        return g.fns.suggestBindings(instance, suggested);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrAttachSessionActionSets(XrSession session,
                                                              const XrSessionActionSetsAttachInfo* info) {
    xrfps::Session* s = find_session(session);
    if (s == nullptr || g.action_set == XR_NULL_HANDLE) {
        return g.fns.attachActionSets(session, info);
    }

    std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
    sets.push_back(g.action_set);

    XrSessionActionSetsAttachInfo merged = *info;
    merged.actionSets = sets.data();
    merged.countActionSets = (uint32_t)sets.size();

    XrResult r = g.fns.attachActionSets(session, &merged);
    if (XR_FAILED(r)) {
        flog("attach with our action set failed (%d), attaching app sets only", r);
        return g.fns.attachActionSets(session, info);
    }

    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space.action = g.grip_action;
    space.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    for (int hand = 0; hand < 2; ++hand) {
        space.subactionPath = g.hand_path[hand];
        g.fns.createActionSpace(session, &space, &s->grip_space[hand]);
    }
    s->our_actionset_attached = true;
    flog("action set attached; grip spaces ready");
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    xrfps::Session* s = find_session(session);
    if (s == nullptr || !s->our_actionset_attached) {
        return g.fns.syncActions(session, info);
    }

    std::vector<XrActiveActionSet> active(info->activeActionSets,
                                          info->activeActionSets + info->countActiveActionSets);
    active.push_back({g.action_set, XR_NULL_PATH});

    XrActionsSyncInfo merged = *info;
    merged.activeActionSets = active.data();
    merged.countActiveActionSets = (uint32_t)active.size();

    const XrResult r = g.fns.syncActions(session, &merged);
    if (r == XR_ERROR_ACTIONSET_NOT_ATTACHED) {
        return g.fns.syncActions(session, info);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrWaitFrame(XrSession session, const XrFrameWaitInfo* wait_info,
                                                XrFrameState* frame_state) {
    perf::timeline::WaitSample wait;
    xrfps::frame_timing::wait_enter(wait);
    const XrResult r = g.fns.waitFrame(session, wait_info, frame_state);
    if (XR_SUCCEEDED(r)) {
        xrfps::Session* s = find_session(session);
        if (s != nullptr) {
            xrfps::session_note_display_period(s, frame_state->predictedDisplayPeriod);
        }
        xrfps::frame_timing::wait_exit(s, wait, *frame_state);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrBeginFrame(XrSession session,
                                                 const XrFrameBeginInfo* begin_info) {
    xrfps::frame_timing::begin_enter();
    const XrResult r = g.fns.beginFrame(session, begin_info);
    xrfps::Session* s = find_session(session);
    if (XR_SUCCEEDED(r) && s != nullptr) {
        xrfps::session_begin_frame(s);
    }
    xrfps::frame_timing::begin_exit(s, XR_SUCCEEDED(r));
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrLocateViews(XrSession session, const XrViewLocateInfo* locate_info,
                                                  XrViewState* view_state, uint32_t capacity,
                                                  uint32_t* count_output, XrView* views) {
    if (locate_info != nullptr && locate_info->type == XR_TYPE_VIEW_LOCATE_INFO) {
        xrfps::frame_timing::locate(locate_info->displayTime);
    }
    return g.fns.locateViews(session, locate_info, view_state, capacity, count_output, views);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    xrfps::Session* s = find_session(session);
    if (s == nullptr || frame_end_info == nullptr) {
        return g.fns.endFrame(session, frame_end_info);
    }
    perf::timeline::FrameRecord frame;
    xrfps::frame_timing::end_enter(s, frame, *frame_end_info);
    const XrResult r = xrfps::session_end_frame(s, frame_end_info);
    xrfps::frame_timing::end_exit(s, frame, *frame_end_info, s->submit_qpc);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrPollEvent(XrInstance instance, XrEventDataBuffer* event_data) {
    const XrResult r = g.fns.pollEvent(instance, event_data);
    if (r == XR_SUCCESS && event_data != nullptr && event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        xrfps::frame_timing::session_state_changed(
            reinterpret_cast<const XrEventDataSessionStateChanged*>(event_data)->state);
    }
    return r;
}

// The app's swapchains only: the HUD's own goes straight to the next layer.
XRAPI_ATTR XrResult XRAPI_CALL hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* create_info,
                                                      XrSwapchain* swapchain) {
    const XrResult r = g.fns.createSwapchain(session, create_info, swapchain);
    if (XR_SUCCEEDED(r) && create_info != nullptr) {
        xrfps::frame_timing::swapchain_created(*create_info);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrDestroyInstance(XrInstance instance) {
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (auto& [handle, s] : g.sessions) {
            xrfps::frame_timing::session_destroyed(s);
            xrfps::session_destroy(s);
        }
        g.sessions.clear();
    }
    xrfps::frame_timing::instance_destroyed();
    const PFN_xrDestroyInstance next_destroy = g.fns.destroyInstance;
    g.instance = XR_NULL_HANDLE;
    g.next_gipa = nullptr;
    g.fns = xrfps::NextFns{};
    g.action_set = XR_NULL_HANDLE;
    g.grip_action = XR_NULL_HANDLE;
    g.runtime_name[0] = '\0';
    return next_destroy(instance);
}

// ---------------------------------------------------------------------------
// Dispatch + negotiation
// ---------------------------------------------------------------------------

// The loader may query these before any instance exists (next_gipa is still
// null then). The layer contributes no extensions of its own.
XRAPI_ATTR XrResult XRAPI_CALL stub_xrEnumerateInstanceExtensionProperties(const char* layer_name,
                                                                           uint32_t capacity,
                                                                           uint32_t* count_output,
                                                                           XrExtensionProperties* properties) {
    (void)layer_name;
    (void)capacity;
    (void)properties;
    if (count_output != nullptr) {
        *count_output = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL stub_xrEnumerateApiLayerProperties(uint32_t capacity, uint32_t* count_output,
                                                                  XrApiLayerProperties* properties) {
    (void)capacity;
    (void)properties;
    if (count_output != nullptr) {
        *count_output = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrfps_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                             PFN_xrVoidFunction* function) {
    static const struct {
        const char* name;
        PFN_xrVoidFunction fn;
    } kHooks[] = {
        {"xrCreateSession", (PFN_xrVoidFunction)hook_xrCreateSession},
        {"xrDestroySession", (PFN_xrVoidFunction)hook_xrDestroySession},
        {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)hook_xrSuggestInteractionProfileBindings},
        {"xrAttachSessionActionSets", (PFN_xrVoidFunction)hook_xrAttachSessionActionSets},
        {"xrSyncActions", (PFN_xrVoidFunction)hook_xrSyncActions},
        {"xrWaitFrame", (PFN_xrVoidFunction)hook_xrWaitFrame},
        {"xrBeginFrame", (PFN_xrVoidFunction)hook_xrBeginFrame},
        {"xrEndFrame", (PFN_xrVoidFunction)hook_xrEndFrame},
        {"xrLocateViews", (PFN_xrVoidFunction)hook_xrLocateViews},
        {"xrPollEvent", (PFN_xrVoidFunction)hook_xrPollEvent},
        {"xrCreateSwapchain", (PFN_xrVoidFunction)hook_xrCreateSwapchain},
        {"xrDestroyInstance", (PFN_xrVoidFunction)hook_xrDestroyInstance},
    };
    for (const auto& hook : kHooks) {
        if (std::strcmp(name, hook.name) == 0) {
            *function = hook.fn;
            return XR_SUCCESS;
        }
    }
    // Only when the runtime has it: the app enabled XR_KHR_vulkan_enable2.
    if (std::strcmp(name, "xrCreateVulkanDeviceKHR") == 0) {
        if (const PFN_xrVoidFunction hook = xrfps::frame_timing::vulkan_device_hook()) {
            *function = hook;
            return XR_SUCCESS;
        }
    }

    if (g.next_gipa == nullptr) {
        // Pre-instance query: only the enumeration entry points are answerable.
        if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) {
            *function = (PFN_xrVoidFunction)stub_xrEnumerateInstanceExtensionProperties;
            return XR_SUCCESS;
        }
        if (std::strcmp(name, "xrEnumerateApiLayerProperties") == 0) {
            *function = (PFN_xrVoidFunction)stub_xrEnumerateApiLayerProperties;
            return XR_SUCCESS;
        }
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return g.next_gipa(instance, name, function);
}

XRAPI_ATTR XrResult XRAPI_CALL xrfps_xrCreateApiLayerInstance(const XrInstanceCreateInfo* create_info,
                                                                const XrApiLayerCreateInfo* layer_info,
                                                                XrInstance* instance) {
    if (layer_info == nullptr || layer_info->nextInfo == nullptr ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const XrApiLayerNextInfo* next = layer_info->nextInfo;

    XrApiLayerCreateInfo chained = *layer_info;
    chained.nextInfo = next->next;

    // Kept so a rejected extension can be retried with the application's own
    // request; its extension-name array belongs to the caller and outlives us.
    const XrInstanceCreateInfo original_create_info = *create_info;
    XrInstanceCreateInfo modified_create_info = *create_info;
    std::vector<const char*> extra_extensions;
    std::vector<std::string> extra_storage;
    if (create_info->enabledExtensionCount > 0) {
        extra_extensions.assign(create_info->enabledExtensionNames,
                                create_info->enabledExtensionNames + create_info->enabledExtensionCount);
    }
    const auto has_extension = [&](const char* wanted) {
        return std::any_of(extra_extensions.begin(), extra_extensions.end(),
                           [wanted](const char* name) { return std::strcmp(name, wanted) == 0; });
    };
    // Meta's telemetry extension gives the HUD runtime-native timing. Whether
    // the runtime has it can only be answered by something below us, and
    // nothing below us can answer until its own instance exists - the loader
    // hands each layer a next-dispatch only once nextCreateApiLayerInstance
    // has returned. Asking earlier reaches a layer that has no downstream yet,
    // which is at best an error and at worst a crash inside that layer. So ask
    // for the extension and let the create tell us: a runtime without it fails
    // the call, and we retry with exactly what the application requested.
    bool added_meta_metrics = false;
    if (!has_extension(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME)) {
        extra_extensions.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
        added_meta_metrics = true;
    }
    // Test aid: XRFPS_TEST_ENABLE_EXT=<name>[,<name>...] enables extra
    // instance extensions on the app's behalf (e.g. Meta XR Operator's
    // automation extension inside an off-the-shelf game). No effect unless
    // the environment variable is set.
    char extra_env[512]{};
    if (GetEnvironmentVariableA("XRFPS_TEST_ENABLE_EXT", extra_env, sizeof(extra_env)) > 0) {
        char* context = nullptr;
        for (char* token = strtok_s(extra_env, ",", &context); token != nullptr;
             token = strtok_s(nullptr, ",", &context)) {
            extra_storage.emplace_back(token);
        }
        for (const std::string& name : extra_storage) {
            if (!has_extension(name.c_str())) {
                extra_extensions.push_back(name.c_str());
            }
        }
    }
    if (extra_extensions.size() != create_info->enabledExtensionCount) {
        modified_create_info.enabledExtensionNames = extra_extensions.data();
        modified_create_info.enabledExtensionCount = (uint32_t)extra_extensions.size();
        create_info = &modified_create_info;
    }

    XrResult r = next->nextCreateApiLayerInstance(create_info, &chained, instance);
    if (XR_FAILED(r) && create_info == &modified_create_info) {
        // One of the extensions we added is unavailable. The application's own
        // request was valid before we touched it, so fall back to it verbatim
        // rather than failing a session the application could have had.
        flog("instance creation failed (%d) with %u added extension(s); retrying with the application's own list",
             (int)r, (unsigned)(extra_extensions.size() - original_create_info.enabledExtensionCount));
        added_meta_metrics = false;
        extra_storage.clear();
        r = next->nextCreateApiLayerInstance(&original_create_info, &chained, instance);
    }
    if (XR_FAILED(r)) {
        return r;
    }
    if (!extra_storage.empty()) {
        flog("enabled %u extra extension(s) via XRFPS_TEST_ENABLE_EXT", (unsigned)extra_storage.size());
    }
    if (added_meta_metrics) {
        flog("enabled %s for runtime timing", XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    }

    g.instance = *instance;
    g.next_gipa = next->nextGetInstanceProcAddr;

    load_next("xrDestroyInstance", g.fns.destroyInstance);
    load_next("xrGetInstanceProperties", g.fns.getInstanceProperties);
    load_next("xrCreateSession", g.fns.createSession);
    load_next("xrDestroySession", g.fns.destroySession);
    load_next("xrSuggestInteractionProfileBindings", g.fns.suggestBindings);
    load_next("xrAttachSessionActionSets", g.fns.attachActionSets);
    load_next("xrSyncActions", g.fns.syncActions);
    load_next("xrWaitFrame", g.fns.waitFrame);
    load_next("xrBeginFrame", g.fns.beginFrame);
    load_next("xrEndFrame", g.fns.endFrame);
    load_next("xrLocateViews", g.fns.locateViews);
    load_next("xrPollEvent", g.fns.pollEvent);
    load_next("xrStringToPath", g.fns.stringToPath);
    load_next("xrCreateActionSet", g.fns.createActionSet);
    load_next("xrDestroyActionSet", g.fns.destroyActionSet);
    load_next("xrCreateAction", g.fns.createAction);
    load_next("xrCreateActionSpace", g.fns.createActionSpace);
    load_next("xrCreateReferenceSpace", g.fns.createReferenceSpace);
    load_next("xrDestroySpace", g.fns.destroySpace);
    load_next("xrLocateSpace", g.fns.locateSpace);
    load_next("xrGetSystemProperties", g.fns.getSystemProperties);
    load_next("xrEnumerateSwapchainFormats", g.fns.enumerateSwapchainFormats);
    load_next("xrCreateSwapchain", g.fns.createSwapchain);
    load_next("xrDestroySwapchain", g.fns.destroySwapchain);
    load_next("xrEnumerateSwapchainImages", g.fns.enumerateSwapchainImages);
    load_next("xrAcquireSwapchainImage", g.fns.acquireSwapchainImage);
    load_next("xrWaitSwapchainImage", g.fns.waitSwapchainImage);
    load_next("xrReleaseSwapchainImage", g.fns.releaseSwapchainImage);
    load_next("xrEnumeratePerformanceMetricsCounterPathsMETA",
              g.fns.enumeratePerformanceMetricsCounterPathsMETA);
    load_next("xrSetPerformanceMetricsStateMETA", g.fns.setPerformanceMetricsStateMETA);
    load_next("xrQueryPerformanceMetricsCounterMETA", g.fns.queryPerformanceMetricsCounterMETA);
    g.fns.getInstanceProcAddr = g.next_gipa;

    if (g.fns.getInstanceProperties != nullptr) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(g.fns.getInstanceProperties(g.instance, &properties))) {
            strncpy_s(g.runtime_name, sizeof(g.runtime_name), properties.runtimeName, _TRUNCATE);
        }
    }

    create_our_actions();
    strncpy_s(g.app_name, sizeof(g.app_name), create_info->applicationInfo.applicationName, _TRUNCATE);
    xrfps::frame_timing::instance_created(g.instance, g.next_gipa, g.app_name);
    flog("instance ready (app: %s, runtime: %s)", create_info->applicationInfo.applicationName,
         g.runtime_name);
    return XR_SUCCESS;
}

} // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info, const char* layer_name,
    XrNegotiateApiLayerRequest* api_layer_request) {
    (void)layer_name;
    if (loader_info == nullptr || api_layer_request == nullptr ||
        loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
        api_layer_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        api_layer_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        api_layer_request->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    api_layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    api_layer_request->layerApiVersion = XR_CURRENT_API_VERSION;
    api_layer_request->getInstanceProcAddr = xrfps_xrGetInstanceProcAddr;
    api_layer_request->createApiLayerInstance = xrfps_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
