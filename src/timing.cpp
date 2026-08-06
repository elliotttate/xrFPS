#include "timing.h"

#include <windows.h>

#include <openvr.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace xrfps {
namespace {

using InitFn = uint32_t(VR_CALLTYPE*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using GetInterfaceFn = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
using ErrorTextFn = const char*(VR_CALLTYPE*)(vr::EVRInitError);

std::wstring registry_string(HKEY root, const wchar_t* key_name, const wchar_t* value_name,
                             REGSAM extra_flags = 0) {
    HKEY key{};
    if (RegOpenKeyExW(root, key_name, 0, KEY_QUERY_VALUE | extra_flags, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t value[2048]{};
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExW(key, value_name, nullptr, &type,
                                         reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[2048]{};
        if (ExpandEnvironmentStringsW(value, expanded, (DWORD)std::size(expanded)) > 0) {
            return expanded;
        }
    }
    return value;
}

void append_candidate(std::vector<std::filesystem::path>& candidates,
                      const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    const auto dll = root / L"bin" / L"win64" / L"openvr_api.dll";
    if (std::find(candidates.begin(), candidates.end(), dll) == candidates.end()) {
        candidates.push_back(dll);
    }
}

std::vector<std::filesystem::path> openvr_candidates() {
    std::vector<std::filesystem::path> result;

    // With SteamVR as the active OpenXR runtime, its manifest lives directly
    // under the SteamVR root. Check both registry views for mixed-bitness
    // installations.
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        const std::wstring manifest = registry_string(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", view);
        if (!manifest.empty()) {
            append_candidate(result, std::filesystem::path(manifest).parent_path());
        }
    }

    std::wstring steam = registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        std::replace(steam.begin(), steam.end(), L'/', L'\\');
        append_candidate(result, std::filesystem::path(steam) / L"steamapps" / L"common" / L"SteamVR");
    }
    return result;
}

bool is_after(uint32_t value, uint32_t previous) {
    return value != previous && (uint32_t)(value - previous) < 0x80000000u;
}

} // namespace

bool SteamVrTiming::start() {
    if (compositor_ != nullptr) {
        return true;
    }
    const uint64_t now = GetTickCount64();
    if (now < next_retry_tick_) {
        return false;
    }
    next_retry_tick_ = now + 2000;

    HMODULE module = GetModuleHandleW(L"openvr_api.dll");
    const bool loaded_by_us = module == nullptr;
    if (module == nullptr) {
        for (const auto& candidate : openvr_candidates()) {
            if (!std::filesystem::exists(candidate)) {
                continue;
            }
            module = LoadLibraryExW(candidate.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (module != nullptr) {
                break;
            }
        }
    }
    if (module == nullptr) {
        status_ = "openvr_api.dll not found";
        return false;
    }

    auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "VR_InitInternal2"));
    auto get_interface =
        reinterpret_cast<GetInterfaceFn>(GetProcAddress(module, "VR_GetGenericInterface"));
    auto error_text = reinterpret_cast<ErrorTextFn>(
        GetProcAddress(module, "VR_GetVRInitErrorAsEnglishDescription"));
    if (init == nullptr || get_interface == nullptr) {
        status_ = "OpenVR entry points unavailable";
        return false;
    }

    vr::EVRInitError error = vr::VRInitError_None;
    // Overlay clients are allowed to inspect the active scene application's
    // compositor history. A Background client receives its own near-empty
    // timing stream instead, which looks like a misleading 0.x ms workload.
    init(&error, vr::VRApplication_Overlay, nullptr);
    if (error != vr::VRInitError_None) {
        status_ = error_text != nullptr ? error_text(error) : "OpenVR initialization failed";
        return false;
    }

    error = vr::VRInitError_None;
    void* compositor = get_interface(vr::IVRCompositor_Version, &error);
    if (compositor == nullptr || error != vr::VRInitError_None) {
        status_ = error_text != nullptr ? error_text(error) : "SteamVR compositor unavailable";
        return false;
    }

    module_ = module;
    compositor_ = compositor;
    owns_runtime_ = loaded_by_us;
    status_ = "SteamVR compositor";
    return true;
}

void SteamVrTiming::stop() {
    if (compositor_ != nullptr && owns_runtime_ && module_ != nullptr) {
        using ShutdownFn = void(VR_CALLTYPE*)();
        if (auto shutdown = reinterpret_cast<ShutdownFn>(
                GetProcAddress(static_cast<HMODULE>(module_), "VR_ShutdownInternal"))) {
            shutdown();
        }
        FreeLibrary(static_cast<HMODULE>(module_));
    }
    module_ = nullptr;
    compositor_ = nullptr;
    owns_runtime_ = false;
    primed_ = false;
    status_ = "not initialized";
}

int SteamVrTiming::collect(RuntimeTimingSample* output, int capacity) {
    if (output == nullptr || capacity <= 0 || !start()) {
        return 0;
    }

    std::array<vr::Compositor_FrameTiming, 128> frames{};
    frames[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
    auto* compositor = static_cast<vr::IVRCompositor*>(compositor_);
    const uint32_t filled = compositor->GetFrameTimings(frames.data(), (uint32_t)frames.size());
    if (filled == 0) {
        status_ = "SteamVR has no application timing yet";
        return 0;
    }

    if (!primed_) {
        const auto& newest = frames[filled - 1];
        last_frame_index_ = newest.m_nFrameIndex;
        last_system_time_ = newest.m_flSystemTimeInSeconds;
        primed_ = true;
        status_ = "SteamVR compositor";
        return 0;
    }

    int written = 0;
    for (uint32_t i = 0; i < filled && written < capacity; ++i) {
        const auto& frame = frames[i];
        if (!is_after(frame.m_nFrameIndex, last_frame_index_)) {
            continue;
        }

        const uint32_t index_delta = frame.m_nFrameIndex - last_frame_index_;
        const double elapsed = (frame.m_flSystemTimeInSeconds - last_system_time_) * 1000.0;
        // A gap is unlikely at the 20 Hz polling rate, but distributing it
        // keeps the rate honest if the process was briefly suspended.
        const float frame_ms = index_delta > 0 ? (float)(elapsed / (double)index_delta) : -1.0f;
        const float cpu_ms = std::max(
            0.0f, frame.m_flNewFrameReadyMs - frame.m_flNewPosesReadyMs +
                      frame.m_flCompositorRenderCpuMs);
        // Valve's documented application-frametime recipe uses the total
        // render GPU field. Pre/post-submit are often zero for OpenXR clients.
        const float gpu_ms = std::max(0.0f, frame.m_flTotalRenderGpuMs);

        if (frame_ms > 0.0f && frame_ms <= 60000.0f) {
            // Preserve all missed application frames as equal intervals rather
            // than silently turning a hitch into one normal-looking frame.
            for (uint32_t gap = 0; gap < index_delta && written < capacity; ++gap) {
                output[written++] = {frame_ms, cpu_ms, gpu_ms};
            }
        }
        last_frame_index_ = frame.m_nFrameIndex;
        last_system_time_ = frame.m_flSystemTimeInSeconds;
    }
    status_ = "SteamVR compositor";
    return written;
}

} // namespace xrfps
