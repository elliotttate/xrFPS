// FlipFPS — implicit OpenXR API layer. Turn a controller upside down to see
// an FPS/frametime HUD composited over any OpenXR application.
//
// Mechanism: the layer appends its own action set (one grip-pose action) to the
// app's suggested bindings / attached action sets / per-frame sync, watches the
// grip orientation at xrEndFrame time, and when flipped injects one extra
// XrCompositionLayerQuad rendered by a tiny CPU rasterizer.
#include "overlay.h"

#include <openxr/openxr_loader_negotiation.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace {

struct Global {
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrGetInstanceProcAddr next_gipa{};
    flipfps::NextFns fns{};

    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction grip_action{XR_NULL_HANDLE};
    XrPath hand_path[2]{XR_NULL_PATH, XR_NULL_PATH};
    XrPath grip_binding[2]{XR_NULL_PATH, XR_NULL_PATH};

    std::mutex mtx;
    std::unordered_map<XrSession, flipfps::Session*> sessions;
};

Global g;

flipfps::Session* find_session(XrSession session) {
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
    strcpy_s(set_info.actionSetName, "flipfps");
    strcpy_s(set_info.localizedActionSetName, "FlipFPS");
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
        auto* s = flipfps::session_create(instance, *session, create_info, &g.fns);
        std::lock_guard<std::mutex> lock(g.mtx);
        g.sessions[*session] = s;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrDestroySession(XrSession session) {
    flipfps::Session* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        const auto it = g.sessions.find(session);
        if (it != g.sessions.end()) {
            s = it->second;
            g.sessions.erase(it);
        }
    }
    flipfps::session_destroy(s);
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
    flipfps::Session* s = find_session(session);
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
    flipfps::Session* s = find_session(session);
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
    const XrResult r = g.fns.waitFrame(session, wait_info, frame_state);
    if (XR_SUCCEEDED(r)) {
        if (flipfps::Session* s = find_session(session)) {
            flipfps::session_note_display_period(s, frame_state->predictedDisplayPeriod);
        }
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    if (flipfps::Session* s = find_session(session)) {
        return flipfps::session_end_frame(s, frame_end_info);
    }
    return g.fns.endFrame(session, frame_end_info);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_xrDestroyInstance(XrInstance instance) {
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (auto& [handle, s] : g.sessions) {
            flipfps::session_destroy(s);
        }
        g.sessions.clear();
    }
    const PFN_xrDestroyInstance next_destroy = g.fns.destroyInstance;
    g.instance = XR_NULL_HANDLE;
    g.next_gipa = nullptr;
    g.fns = flipfps::NextFns{};
    g.action_set = XR_NULL_HANDLE;
    g.grip_action = XR_NULL_HANDLE;
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

XRAPI_ATTR XrResult XRAPI_CALL flipfps_xrGetInstanceProcAddr(XrInstance instance, const char* name,
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
        {"xrEndFrame", (PFN_xrVoidFunction)hook_xrEndFrame},
        {"xrDestroyInstance", (PFN_xrVoidFunction)hook_xrDestroyInstance},
    };
    for (const auto& hook : kHooks) {
        if (std::strcmp(name, hook.name) == 0) {
            *function = hook.fn;
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

XRAPI_ATTR XrResult XRAPI_CALL flipfps_xrCreateApiLayerInstance(const XrInstanceCreateInfo* create_info,
                                                                const XrApiLayerCreateInfo* layer_info,
                                                                XrInstance* instance) {
    if (layer_info == nullptr || layer_info->nextInfo == nullptr ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const XrApiLayerNextInfo* next = layer_info->nextInfo;

    XrApiLayerCreateInfo chained = *layer_info;
    chained.nextInfo = next->next;

    const XrResult r = next->nextCreateApiLayerInstance(create_info, &chained, instance);
    if (XR_FAILED(r)) {
        return r;
    }

    g.instance = *instance;
    g.next_gipa = next->nextGetInstanceProcAddr;

    load_next("xrDestroyInstance", g.fns.destroyInstance);
    load_next("xrCreateSession", g.fns.createSession);
    load_next("xrDestroySession", g.fns.destroySession);
    load_next("xrSuggestInteractionProfileBindings", g.fns.suggestBindings);
    load_next("xrAttachSessionActionSets", g.fns.attachActionSets);
    load_next("xrSyncActions", g.fns.syncActions);
    load_next("xrWaitFrame", g.fns.waitFrame);
    load_next("xrEndFrame", g.fns.endFrame);
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
    g.fns.getInstanceProcAddr = g.next_gipa;

    create_our_actions();
    flog("instance ready (app: %s)", create_info->applicationInfo.applicationName);
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
    api_layer_request->getInstanceProcAddr = flipfps_xrGetInstanceProcAddr;
    api_layer_request->createApiLayerInstance = flipfps_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
