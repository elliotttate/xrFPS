#include "metrics_collector.h"

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace flipfps {
namespace {

// NVML, resolved at runtime so the build has no NVIDIA dependency.
using nvmlReturn_t = int;
using nvmlDevice_t = void*;
struct nvmlUtilization_t {
    unsigned int gpu, memory;
};
struct nvmlMemory_t {
    unsigned long long total, free, used;
};

using PFN_nvmlInit = nvmlReturn_t(*)();
using PFN_nvmlShutdown = nvmlReturn_t(*)();
using PFN_nvmlDeviceGetHandleByIndex = nvmlReturn_t(*)(unsigned int, nvmlDevice_t*);
using PFN_nvmlDeviceGetUtilizationRates = nvmlReturn_t(*)(nvmlDevice_t, nvmlUtilization_t*);
using PFN_nvmlDeviceGetMemoryInfo = nvmlReturn_t(*)(nvmlDevice_t, nvmlMemory_t*);
using PFN_nvmlDeviceGetTemperature = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned int*);
using PFN_nvmlDeviceGetClockInfo = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned int*);

struct Nvml {
    PFN_nvmlInit init{};
    PFN_nvmlShutdown shutdown{};
    PFN_nvmlDeviceGetHandleByIndex get_handle{};
    PFN_nvmlDeviceGetUtilizationRates get_util{};
    PFN_nvmlDeviceGetMemoryInfo get_mem{};
    PFN_nvmlDeviceGetTemperature get_temp{};
    PFN_nvmlDeviceGetClockInfo get_clock{};
};

Nvml g_nvml;

// Known VR runtime/game host patterns, most specific first. The collector
// reports GPU load for the actual VR app rather than the whole desktop.
bool looks_like_vr_app(const wchar_t* exe) {
    static const wchar_t* kSkip[] = {L"vrserver.exe",   L"vrcompositor.exe", L"vrmonitor.exe",
                                     L"vrdashboard.exe", L"vrwebhelper.exe",  L"steam.exe",
                                     L"steamwebhelper.exe", L"explorer.exe", L"dwm.exe",
                                     L"OVRServer_x64.exe", L"OVRRedir.exe", L"MetaXRSimulator.exe",
                                     L"VirtualDesktop.Streamer.exe", L"flipfps_control.exe"};
    for (const wchar_t* skip : kSkip) {
        if (_wcsicmp(exe, skip) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

MetricsCollector::~MetricsCollector() { stop(); }

bool MetricsCollector::start() {
    if (view_ != nullptr) {
        return true;
    }

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(SharedMetrics), SharedMetrics::kName);
    if (mapping == nullptr) {
        status_ = "cannot create shared memory";
        return false;
    }
    auto* view = (SharedMetrics*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMetrics));
    if (view == nullptr) {
        CloseHandle(mapping);
        status_ = "cannot map shared memory";
        return false;
    }
    *view = SharedMetrics{};
    mapping_ = mapping;
    view_ = view;

    // PDH: total CPU plus every GPU 3D engine instance (summed per process).
    PDH_HQUERY query{};
    if (PdhOpenQueryA(nullptr, 0, &query) == ERROR_SUCCESS) {
        pdh_query_ = query;
        PDH_HCOUNTER counter{};
        if (PdhAddEnglishCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) ==
            ERROR_SUCCESS) {
            counter_cpu_ = counter;
        }
        if (PdhAddEnglishCounterA(query, "\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0,
                                  &counter) == ERROR_SUCCESS) {
            counter_gpu_ = counter;
        }
        PdhCollectQueryData(query); // first call primes the deltas
    }

    // NVML is optional; without it the NVIDIA-specific rows stay hidden.
    if (g_nvml.init == nullptr) {
        HMODULE lib = LoadLibraryA("nvml.dll");
        if (lib == nullptr) {
            lib = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
        if (lib != nullptr) {
            nvml_lib_ = lib;
            g_nvml.init = (PFN_nvmlInit)GetProcAddress(lib, "nvmlInit_v2");
            g_nvml.shutdown = (PFN_nvmlShutdown)GetProcAddress(lib, "nvmlShutdown");
            g_nvml.get_handle =
                (PFN_nvmlDeviceGetHandleByIndex)GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
            g_nvml.get_util =
                (PFN_nvmlDeviceGetUtilizationRates)GetProcAddress(lib, "nvmlDeviceGetUtilizationRates");
            g_nvml.get_mem = (PFN_nvmlDeviceGetMemoryInfo)GetProcAddress(lib, "nvmlDeviceGetMemoryInfo");
            g_nvml.get_temp = (PFN_nvmlDeviceGetTemperature)GetProcAddress(lib, "nvmlDeviceGetTemperature");
            g_nvml.get_clock = (PFN_nvmlDeviceGetClockInfo)GetProcAddress(lib, "nvmlDeviceGetClockInfo");
        }
    }
    if (g_nvml.init != nullptr && g_nvml.init() == 0 && g_nvml.get_handle != nullptr) {
        nvmlDevice_t dev{};
        if (g_nvml.get_handle(0, &dev) == 0) {
            nvml_device_ = dev;
        }
    }

    status_ = nvml_device_ != nullptr ? "running (PDH + NVML)" : "running (PDH)";
    return true;
}

void MetricsCollector::stop() {
    if (pdh_query_ != nullptr) {
        PdhCloseQuery((PDH_HQUERY)pdh_query_);
        pdh_query_ = nullptr;
        counter_cpu_ = nullptr;
        counter_gpu_ = nullptr;
    }
    if (nvml_device_ != nullptr && g_nvml.shutdown != nullptr) {
        g_nvml.shutdown();
        nvml_device_ = nullptr;
    }
    if (nvml_lib_ != nullptr) {
        FreeLibrary((HMODULE)nvml_lib_);
        nvml_lib_ = nullptr;
        g_nvml = Nvml{};
    }
    if (session_view_ != nullptr) {
        UnmapViewOfFile((const void*)session_view_);
        session_view_ = nullptr;
    }
    if (session_mapping_ != nullptr) {
        CloseHandle((HANDLE)session_mapping_);
        session_mapping_ = nullptr;
    }
    if (view_ != nullptr) {
        view_->updated_tick = 0; // readers see "stale" immediately
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle((HANDLE)mapping_);
        mapping_ = nullptr;
    }
    status_ = "stopped";
}

void MetricsCollector::detect_vr_app() {
    // The layer publishes the app it is loaded into. Reading one shared struct
    // replaces walking every process's module list, which cost hundreds of
    // milliseconds per sample whenever no VR app was running.
    if (session_view_ == nullptr) {
        session_mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, SharedSession::kName);
        if (session_mapping_ != nullptr) {
            session_view_ = (const SharedSession*)MapViewOfFile(session_mapping_, FILE_MAP_READ, 0, 0,
                                                                sizeof(SharedSession));
        }
    }

    if (session_view_ != nullptr && session_view_->fresh(GetTickCount64())) {
        vr_pid_ = session_view_->pid;
        vr_name_ = session_view_->app_name;
    } else {
        vr_pid_ = 0;
        vr_name_.clear();
    }
}

void MetricsCollector::sample() {
    if (view_ == nullptr) {
        return;
    }

    detect_vr_app();
    latest_.app_pid = vr_pid_;
    std::snprintf(latest_.app_name, sizeof(latest_.app_name), "%s", vr_name_.c_str());

    if (pdh_query_ != nullptr && PdhCollectQueryData((PDH_HQUERY)pdh_query_) == ERROR_SUCCESS) {
        if (!pdh_primed_) {
            pdh_primed_ = true; // the first interval has no delta to report
        } else {
            if (counter_cpu_ != nullptr) {
                PDH_FMT_COUNTERVALUE value{};
                if (PdhGetFormattedCounterValue((PDH_HCOUNTER)counter_cpu_, PDH_FMT_DOUBLE, nullptr,
                                                &value) == ERROR_SUCCESS) {
                    latest_.cpu_usage_pct = (float)value.doubleValue;
                }
            }
            if (counter_gpu_ != nullptr) {
                DWORD size = 0, count = 0;
                if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)counter_gpu_, PDH_FMT_DOUBLE, &size, &count,
                                                 nullptr) == PDH_MORE_DATA) {
                    std::vector<uint8_t> buffer(size);
                    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)buffer.data();
                    if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)counter_gpu_, PDH_FMT_DOUBLE, &size,
                                                     &count, items) == ERROR_SUCCESS) {
                        // Instances are per process and per engine; the whole-GPU
                        // figure is their sum, and the app's is the subset whose
                        // instance name carries its pid.
                        char pid_tag[32]{};
                        std::snprintf(pid_tag, sizeof(pid_tag), "pid_%lu_", vr_pid_);
                        double total = 0.0;
                        double app = 0.0;
                        for (DWORD i = 0; i < count; ++i) {
                            const double v = items[i].FmtValue.doubleValue;
                            if (v <= 0.0) {
                                continue;
                            }
                            total += v;
                            if (vr_pid_ != 0 && items[i].szName != nullptr &&
                                std::strstr(items[i].szName, pid_tag) != nullptr) {
                                app += v;
                            }
                        }
                        latest_.gpu_usage_pct = (float)std::min(total, 100.0);
                        latest_.gpu_app_usage_pct = vr_pid_ != 0 ? (float)std::min(app, 100.0) : -1.0f;
                    }
                }
            }
        }
    }

    if (nvml_device_ != nullptr) {
        if (g_nvml.get_util != nullptr) {
            nvmlUtilization_t util{};
            if (g_nvml.get_util(nvml_device_, &util) == 0) {
                // NVML's whole-GPU number is more accurate than summing PDH.
                latest_.gpu_usage_pct = (float)util.gpu;
            }
        }
        if (g_nvml.get_mem != nullptr) {
            nvmlMemory_t mem{};
            if (g_nvml.get_mem(nvml_device_, &mem) == 0) {
                latest_.vram_used_mb = (float)(mem.used / (1024.0 * 1024.0));
                latest_.vram_total_mb = (float)(mem.total / (1024.0 * 1024.0));
            }
        }
        if (g_nvml.get_temp != nullptr) {
            unsigned int temp = 0;
            if (g_nvml.get_temp(nvml_device_, 0 /* NVML_TEMPERATURE_GPU */, &temp) == 0) {
                latest_.gpu_temp_c = (float)temp;
            }
        }
        if (g_nvml.get_clock != nullptr) {
            unsigned int clock = 0;
            if (g_nvml.get_clock(nvml_device_, 1 /* NVML_CLOCK_SM */, &clock) == 0) {
                latest_.gpu_clock_mhz = (float)clock;
            }
        }
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        latest_.ram_total_gb = (float)(mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        latest_.ram_used_gb =
            (float)((mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
    }

    latest_.updated_tick = GetTickCount64();
    *view_ = latest_;
}

} // namespace flipfps
