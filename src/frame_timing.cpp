#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// The bindings this file reads; overlay.h adds Win32, D3D11 and D3D12.
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#include "overlay.h"

#include "frame_timing.h"
#include "gpu_frame_timer.h"
#include "log.h"
#include "vulkan_frame_timer.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef XRFPS_VERSION
#define XRFPS_VERSION "dev"
#endif

namespace xrfps {

namespace tl = perf::timeline;

enum class GpuApiKind { None, D3D11, D3D12, Vulkan, OpenGL };

// Per-session GPU timing state: the app's device and queue or context, and
// the timer that stamps it.
struct SessionTiming {
    GpuApiKind api{GpuApiKind::None};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue;
    VkInstance vk_instance{VK_NULL_HANDLE};
    VkPhysicalDevice vk_physical{VK_NULL_HANDLE};
    VkDevice vk_device{VK_NULL_HANDLE};
    uint32_t vk_family{0};
    uint32_t vk_index{0};
    HGLRC gl_context{};

    perf::gpu::D3D12Timer d3d12;
    perf::gpu::D3D11Timer d3d11;
    perf::gpu::VulkanTimer vulkan;
    perf::gpu::GlTimer gl;

    bool timer_started{false};  // set up, or failed to be
    bool gl_tried{false};       // OpenGL's starts inside a frame call, once
    bool frame_timed{false};    // the frame in flight carries GPU timestamps
    const char* timer_text{"off"};  // what the session info says while timing is on
    // D3D11's immediate context is not thread-safe. xrEndFrame is where the
    // app's frame thread is known to own it, so xrBeginFrame only stamps from there.
    std::atomic<DWORD> d3d11_frame_thread{0};
    double refresh_hz{0.0};
};

namespace frame_timing {
namespace {

tl::Recorder g_recorder;
std::atomic<uint64_t> g_frame_count{0};
std::atomic<bool> g_info_dirty{true};
std::atomic<int> g_session_state{XR_SESSION_STATE_UNKNOWN};

std::mutex g_info_mutex;  // the strings and swapchain below
std::string g_runtime_name;
std::string g_runtime_version;
std::string g_system_name;
std::string g_graphics_api;
std::string g_gpu_json{"null"};
std::string g_gpu_timing{"off"};
XrSwapchainCreateInfo g_app_swapchain{XR_TYPE_SWAPCHAIN_CREATE_INFO};

PFN_xrCreateVulkanDeviceKHR g_next_create_vulkan_device{};
std::mutex g_vulkan_mutex;
std::vector<VkDevice> g_calibrated_devices;  // created with calibrated timestamps

bool env_flag(const char* name, int& value) {
    char text[16]{};
    const DWORD length = GetEnvironmentVariableA(name, text, sizeof(text));
    if (length == 0 || length >= sizeof(text)) return false;
    value = atoi(text) != 0 ? 1 : 0;
    return true;
}

bool timeline_wanted() {
    static const bool wanted = [] {
        int value = 1;
        return !env_flag("XRFPS_FRAME_TIMELINE", value) || value != 0;
    }();
    return wanted;
}

// The Control Center's setting, unless XRFPS_GPU_TIMING pins it.
bool gpu_timing_setting(bool configured) {
    static const int forced = [] {
        int value = 0;
        return env_flag("XRFPS_GPU_TIMING", value) ? value : -1;
    }();
    return forced >= 0 ? forced != 0 : configured;
}

bool gpu_timing_wanted(const Session* s) {
    return gpu_timing_setting(s->config.gpu_timing) && g_recorder.Enabled();
}

std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}

// The adapter behind a D3D device, by LUID. Described once per session.
std::string gpu_json(LUID luid) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc))) {
        return "null";
    }
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    char driver[48] = "unknown";
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        snprintf(driver, sizeof(driver), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                 HIWORD(umd.LowPart), LOWORD(umd.LowPart));
    }
    char json[512];
    snprintf(json, sizeof(json),
             "{\"name\": %s, \"vendor_id\": %u, \"device_id\": %u, \"driver\": \"%s\", \"vram_mb\": %llu}",
             json_text(name).c_str(), desc.VendorId, desc.DeviceId, driver,
             (unsigned long long)(desc.DedicatedVideoMemory >> 20));
    return json;
}

const char* api_name(GpuApiKind api) {
    switch (api) {
        case GpuApiKind::D3D11: return "D3D11";
        case GpuApiKind::D3D12: return "D3D12";
        case GpuApiKind::Vulkan: return "Vulkan";
        case GpuApiKind::OpenGL: return "OpenGL";
        default: return "unknown";
    }
}

void set_gpu_timing_text(const char* text) {
    std::lock_guard<std::mutex> lock(g_info_mutex);
    if (g_gpu_timing != text) {
        g_gpu_timing = text;
        g_info_dirty = true;
    }
}

// What a capture should say about the run it came from. xrEndFrame is the
// timeline's one writer, so this runs there.
void publish_session_info() {
    std::string json;
    {
        std::lock_guard<std::mutex> lock(g_info_mutex);
        char numbers[512];
        snprintf(numbers, sizeof(numbers),
                 "\"app_swapchain\": {\"width\": %u, \"height\": %u, \"format\": %lld, \"samples\": %u}",
                 g_app_swapchain.width, g_app_swapchain.height, (long long)g_app_swapchain.format,
                 g_app_swapchain.sampleCount);
        const double hz = g_recorder.Ring().Header() ? g_recorder.Ring().Header()->refreshHz : 0.0;
        char rate[64];
        snprintf(rate, sizeof(rate), "%g", hz);
        json = "{\"source\": {\"kind\": \"layer\", \"name\": \"xrFPS\", \"version\": " + json_text(XRFPS_VERSION) +
               "}, \"runtime\": {\"name\": " + json_text(g_runtime_name) +
               ", \"version\": " + json_text(g_runtime_version) + "}, \"system\": {\"name\": " +
               json_text(g_system_name) + "}, \"graphics_api\": " + json_text(g_graphics_api) +
               ", \"gpu\": " + g_gpu_json + ", \"refresh_hz\": " + rate + ", " + numbers +
               ", \"gpu_timing\": " + json_text(g_gpu_timing) + "}";
    }
    g_recorder.SetSessionInfo(json);
}

const char* state_name(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

bool gl_current(const SessionTiming& t) {
    return t.api == GpuApiKind::OpenGL && t.gl_context && wglGetCurrentContext() == t.gl_context;
}

void start_timer(SessionTiming& t) {
    t.timer_started = true;
    bool on = false;
    switch (t.api) {
        case GpuApiKind::D3D12:
            on = t.d3d12.Init(t.d3d12_device.Get(), t.d3d12_queue.Get());
            t.timer_text = on ? "D3D12 timestamps on the CPU clock" : "unavailable";
            break;
        case GpuApiKind::D3D11:
            on = t.d3d11.Init(t.d3d11_device.Get(), t.d3d11_context.Get());
            t.timer_text = on ? "D3D11 durations only" : "unavailable";
            break;
        case GpuApiKind::Vulkan: {
            bool calibrated = false;
            {
                std::lock_guard<std::mutex> lock(g_vulkan_mutex);
                calibrated = std::find(g_calibrated_devices.begin(), g_calibrated_devices.end(), t.vk_device) !=
                             g_calibrated_devices.end();
            }
            on = t.vulkan.Init(t.vk_instance, t.vk_physical, t.vk_device, t.vk_family, t.vk_index, calibrated);
            t.timer_text = !on ? "unavailable"
                               : t.vulkan.Calibrated() ? "Vulkan timestamps on the CPU clock"
                                                       : "Vulkan durations only";
            break;
        }
        case GpuApiKind::OpenGL:
            return;  // starts inside a frame call, where the app's context is current
        default:
            t.timer_text = "unavailable";
            return;
    }
    flog("frame timeline: GPU timing %s (%s)", on ? "on" : "unavailable", api_name(t.api));
}

void stop_timer(SessionTiming& t) {
    t.d3d12.Shutdown();
    t.d3d11 = perf::gpu::D3D11Timer{};
    t.vulkan.Shutdown();
    t.gl = perf::gpu::GlTimer{};  // its queries die with the app's context
    t.timer_started = false;
    t.gl_tried = false;
    t.frame_timed = false;
    t.d3d11_frame_thread = 0;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_create_vulkan_device(XrInstance instance,
                                                         const XrVulkanDeviceCreateInfoKHR* create_info,
                                                         VkDevice* device, VkResult* vk_result) {
    if (!g_next_create_vulkan_device) return XR_ERROR_FUNCTION_UNSUPPORTED;
    // No session exists yet, so read the setting itself. With GPU timing off,
    // xrFPS leaves the app's device exactly as the app asked for it.
    Config config;
    config.load();
    if (!create_info || !create_info->vulkanCreateInfo || !g_recorder.Enabled() ||
        !gpu_timing_setting(config.gpu_timing)) {
        return g_next_create_vulkan_device(instance, create_info, device, vk_result);
    }
    const VkDeviceCreateInfo& app = *create_info->vulkanCreateInfo;
    std::vector<const char*> names(app.ppEnabledExtensionNames, app.ppEnabledExtensionNames + app.enabledExtensionCount);
    const auto has = [&](const char* wanted) {
        return std::any_of(names.begin(), names.end(), [&](const char* n) { return std::strcmp(n, wanted) == 0; });
    };
    bool calibrated = has("VK_KHR_calibrated_timestamps") || has("VK_EXT_calibrated_timestamps");
    const char* added = nullptr;
    if (!calibrated && create_info->vulkanPhysicalDevice) {
        // The loader exports this trampoline; it needs no VkInstance.
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        auto enumerate = loader ? reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                                      GetProcAddress(loader, "vkEnumerateDeviceExtensionProperties"))
                                : nullptr;
        uint32_t count = 0;
        if (enumerate && enumerate(create_info->vulkanPhysicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> offered(count);
            enumerate(create_info->vulkanPhysicalDevice, nullptr, &count, offered.data());
            for (const char* candidate : {"VK_KHR_calibrated_timestamps", "VK_EXT_calibrated_timestamps"}) {
                if (std::any_of(offered.begin(), offered.end(),
                                [&](const VkExtensionProperties& p) { return std::strcmp(p.extensionName, candidate) == 0; })) {
                    added = candidate;
                    break;
                }
            }
        }
    }
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    if (added) {
        names.push_back(added);
        VkDeviceCreateInfo modified_device = app;
        modified_device.enabledExtensionCount = uint32_t(names.size());
        modified_device.ppEnabledExtensionNames = names.data();
        XrVulkanDeviceCreateInfoKHR modified = *create_info;
        modified.vulkanCreateInfo = &modified_device;
        result = g_next_create_vulkan_device(instance, &modified, device, vk_result);
        if (XR_SUCCEEDED(result) && *vk_result == VK_SUCCESS) {
            calibrated = true;
        } else {
            flog("frame timeline: device creation with %s failed; retrying without it", added);
            result = XR_ERROR_RUNTIME_FAILURE;
        }
    }
    if (result == XR_ERROR_RUNTIME_FAILURE) result = g_next_create_vulkan_device(instance, create_info, device, vk_result);
    if (XR_SUCCEEDED(result) && *vk_result == VK_SUCCESS && calibrated) {
        std::lock_guard<std::mutex> lock(g_vulkan_mutex);
        g_calibrated_devices.push_back(*device);
    }
    return result;
}

} // namespace

void instance_created(XrInstance instance, PFN_xrGetInstanceProcAddr next_gipa, const char* app_name) {
    if (timeline_wanted() && !g_recorder.Enabled()) {
        if (g_recorder.Open(0.0)) {
            flog("frame timeline: Local\\xrFPS.FrameTimeline.%lu", GetCurrentProcessId());
            // The loader unloads layers with their instance; a viewer holding
            // the mapping open keeps the frames from before, so carry on their ids.
            const uint64_t last = g_recorder.Ring().LastFrameId();
            if (last > g_frame_count.load()) g_frame_count = last;
        } else {
            flog("frame timeline unavailable (error %lu)", GetLastError());
        }
    }
    g_next_create_vulkan_device = nullptr;
    if (next_gipa) {
        next_gipa(instance, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&g_next_create_vulkan_device));
        PFN_xrGetInstanceProperties properties_fn = nullptr;
        next_gipa(instance, "xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction*>(&properties_fn));
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (properties_fn && XR_SUCCEEDED(properties_fn(instance, &properties))) {
            char version[48];
            snprintf(version, sizeof(version), "%u.%u.%u", XR_VERSION_MAJOR(properties.runtimeVersion),
                     XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
            std::lock_guard<std::mutex> lock(g_info_mutex);
            g_runtime_name = properties.runtimeName;
            g_runtime_version = version;
        }
    }
    g_recorder.SetApplicationName(app_name);
    g_recorder.Marker(tl::MarkerKind::Instance, 0, 0, app_name);
    g_info_dirty = true;
}

void instance_destroyed() {
    g_next_create_vulkan_device = nullptr;
    std::lock_guard<std::mutex> lock(g_vulkan_mutex);
    g_calibrated_devices.clear();
}

void session_created(Session* s, XrInstance instance, const XrSessionCreateInfo* create_info) {
    if (!s) return;
    auto* t = new SessionTiming();
    s->timing = t;
    for (const auto* entry = reinterpret_cast<const XrBaseInStructure*>(create_info->next); entry;
         entry = entry->next) {
        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
            t->api = GpuApiKind::D3D11;
            t->d3d11_device = b->device;
            b->device->GetImmediateContext(&t->d3d11_context);
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* b = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
            t->api = GpuApiKind::D3D12;
            t->d3d12_device = b->device;
            t->d3d12_queue = b->queue;
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {  // also vulkan_enable2's
            const auto* b = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(entry);
            t->api = GpuApiKind::Vulkan;
            t->vk_instance = b->instance;
            t->vk_physical = b->physicalDevice;
            t->vk_device = b->device;
            t->vk_family = b->queueFamilyIndex;
            t->vk_index = b->queueIndex;
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
            const auto* b = reinterpret_cast<const XrGraphicsBindingOpenGLWin32KHR*>(entry);
            t->api = GpuApiKind::OpenGL;
            t->gl_context = b->hGLRC;
        }
    }

    std::string gpu = "null";
    if (t->d3d12_device) {
        gpu = gpu_json(t->d3d12_device->GetAdapterLuid());
    } else if (t->d3d11_device) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(t->d3d11_device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc))) {
            gpu = gpu_json(desc.AdapterLuid);
        }
    }
    XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
    const bool has_system = create_info->systemId != XR_NULL_SYSTEM_ID && s->fns->getSystemProperties &&
                            XR_SUCCEEDED(s->fns->getSystemProperties(instance, create_info->systemId, &system));
    {
        std::lock_guard<std::mutex> lock(g_info_mutex);
        g_graphics_api = api_name(t->api);
        g_gpu_json = gpu;
        g_system_name = has_system ? system.systemName : "";
        g_gpu_timing = "off";
        g_app_swapchain = XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    }
    g_info_dirty = true;
    if (gpu_timing_wanted(s)) start_timer(*t);
}

void session_destroyed(Session* s, bool device_may_be_gone) {
    if (!s || !s->timing) return;
    // D3D devices are held by reference; a Vulkan device is not, and an app
    // that skips xrDestroySession may destroy it before its instance.
    if (device_may_be_gone) s->timing->vulkan.Abandon();
    stop_timer(*s->timing);
    delete s->timing;
    s->timing = nullptr;
}

void session_state_changed(XrSessionState state) {
    g_session_state = state;
    g_recorder.Marker(tl::MarkerKind::SessionState, state, 0, state_name(state));
    g_info_dirty = true;
}

void swapchain_created(const XrSwapchainCreateInfo& info) {
    char detail[80];
    snprintf(detail, sizeof(detail), "format=%lld samples=%u array=%u", (long long)info.format, info.sampleCount,
             info.arraySize);
    g_recorder.Marker(tl::MarkerKind::SwapchainCreate, info.width, info.height, detail);
    if (!(info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) return;
    // The eye buffers: the largest colour swapchain, not whichever came last
    // (often a small UI quad's).
    std::lock_guard<std::mutex> lock(g_info_mutex);
    if (uint64_t(info.width) * info.height >= uint64_t(g_app_swapchain.width) * g_app_swapchain.height) {
        g_app_swapchain = info;
        g_app_swapchain.next = nullptr;
        g_info_dirty = true;
    }
}

void settings_changed() {
    g_recorder.Marker(tl::MarkerKind::UiCommand, 0, 0, "xrFPS settings");
    g_info_dirty = true;
}

void wait_enter(tl::WaitSample& wait) {
    // The runtime paces inside its own xrWaitFrame: all of the call is the wait.
    g_recorder.WaitEnter(wait);
    g_recorder.WaitPace(wait);
}

void wait_exit(Session* s, tl::WaitSample& wait, const XrFrameState& state) {
    g_recorder.WaitExit(wait, 0, tl::Qpc(), state.predictedDisplayTime, state.predictedDisplayPeriod, false);
    if (!s || !s->timing || s->period_floor_ms <= 0.5f) return;
    // The panel's rate, from the short end of recent display periods (see
    // session_note_display_period): half-rate pacing hints must not halve it.
    // The unsmoothed reading: the HUD's target holds still within 5%.
    const double hz = 1000.0 / double(s->period_floor_ms);
    if (std::abs(hz - s->timing->refresh_hz) > s->timing->refresh_hz * 0.005) {
        s->timing->refresh_hz = hz;
        g_recorder.SetRefreshHz(hz);
        g_info_dirty = true;
    }
}

void wait_failed() { g_recorder.WaitAbandoned(); }

void begin_enter() { g_recorder.Begin(); }

void begin_exit(Session* s, bool succeeded) {
    g_recorder.BeginExit();
    if (!succeeded || !s || !s->timing) return;
    SessionTiming& t = *s->timing;
    t.frame_timed = gpu_timing_wanted(s);
    if (!t.frame_timed) return;
    if (!t.timer_started) start_timer(t);
    switch (t.api) {
        case GpuApiKind::D3D12: if (t.d3d12.Active()) t.d3d12.Begin(); break;
        case GpuApiKind::Vulkan: t.vulkan.Begin(); break;
        case GpuApiKind::D3D11:
            if (t.d3d11.Active() && GetCurrentThreadId() == t.d3d11_frame_thread.load()) t.d3d11.Begin();
            break;
        case GpuApiKind::OpenGL:
            if (!gl_current(t)) break;
            if (!t.gl.Active() && !t.gl_tried) {
                t.gl_tried = true;
                const bool on = t.gl.Init();
                t.timer_text = on ? "OpenGL timestamps on the CPU clock" : "unavailable";
                flog("frame timeline: GPU timing %s (OpenGL)", on ? "on" : "unavailable");
            }
            if (t.gl.Active()) t.gl.Begin();
            break;
        default: break;
    }
}

void locate(XrTime display_time) { g_recorder.Locate(display_time); }

void end_enter(Session* s, tl::FrameRecord& frame, const XrFrameEndInfo& info) {
    (void)info;
    frame = tl::FrameRecord{};
    g_recorder.EndEnter(frame, tl::Qpc());
    frame.frameId = ++g_frame_count;
    if (!g_recorder.Enabled()) return;
    if (s && s->timing) {
        SessionTiming& t = *s->timing;
        if (t.frame_timed) {
            // Behind the app's frame on its queue, ahead of the HUD upload.
            switch (t.api) {
                case GpuApiKind::D3D12: if (t.d3d12.Active()) t.d3d12.End(frame.frameId); break;
                case GpuApiKind::Vulkan: t.vulkan.End(frame.frameId); break;
                case GpuApiKind::D3D11:
                    if (t.d3d11.Active()) {
                        t.d3d11_frame_thread = GetCurrentThreadId();
                        t.d3d11.End(frame.frameId);
                    }
                    break;
                case GpuApiKind::OpenGL: if (t.gl.Active() && gl_current(t)) t.gl.End(frame.frameId); break;
                default: break;
            }
        }
        // Earlier frames' results. Gathered here, before the runtime's
        // submission, so the time shows as xrFPS's rather than the runtime's.
        static std::vector<tl::GpuRecord> results;
        results.clear();
        switch (t.api) {
            case GpuApiKind::D3D12: if (t.d3d12.Active()) t.d3d12.Collect(results); break;
            case GpuApiKind::D3D11: if (t.d3d11.Active()) t.d3d11.Collect(results); break;
            case GpuApiKind::Vulkan: t.vulkan.Collect(results); break;
            case GpuApiKind::OpenGL: if (t.gl.Active() && gl_current(t)) t.gl.Collect(results); break;
            default: break;
        }
        for (const tl::GpuRecord& r : results) g_recorder.Gpu(r);
        set_gpu_timing_text(t.frame_timed ? t.timer_text : "off");
    }
    if (g_info_dirty.exchange(false)) publish_session_info();
}

void end_exit(Session* s, tl::FrameRecord& frame, const XrFrameEndInfo& info, XrResult result) {
    if (!g_recorder.Enabled()) return;
    // A frame the runtime refused was never shown.
    if (XR_FAILED(result) || !s) {
        g_recorder.EndAbandoned();
        return;
    }
    frame.endPaintQpc = s->submit_qpc;  // xrFPS's own work done; the runtime's submission follows
    frame.layerCount = info.layerCount;
    uint32_t projection = 0, quad = 0, cylinder = 0, other = 0;
    bool stereo = false;
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = info.layers ? info.layers[i] : nullptr;
        if (!layer) continue;
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            ++projection;
            stereo |= reinterpret_cast<const XrCompositionLayerProjection*>(layer)->viewCount == 2;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            ++quad;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            ++cylinder;
        } else {
            ++other;
        }
    }
    frame.projectionLayers = uint16_t(projection);
    frame.quadLayers = uint16_t(quad);
    frame.cylinderLayers = uint16_t(cylinder);
    frame.otherLayers = uint16_t(other);
    uint32_t flags = 0;
    if (stereo) flags |= tl::kFrameStereoProjection;
    if (projection == 0) flags |= tl::kFrameNoProjection;
    if (g_session_state.load() == XR_SESSION_STATE_FOCUSED) flags |= tl::kFrameSessionFocused;
    frame.flags = flags;
    // When the runtime's call returned, not now: what xrFPS does after it is not the runtime's.
    g_recorder.EndExit(frame, info.displayTime, s->end_return_qpc);
}

PFN_xrVoidFunction vulkan_device_hook() {
    return g_next_create_vulkan_device ? reinterpret_cast<PFN_xrVoidFunction>(hook_create_vulkan_device) : nullptr;
}

} // namespace frame_timing
} // namespace xrfps
