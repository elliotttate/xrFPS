// xrFPS Control Center — install/enable the layer, tune every option, pick
// which readouts appear, and preview the result. Settings apply live: the
// layer re-reads the config file about once a second, so a game already
// running picks up changes without restarting.
#include <windows.h>

#include <d3d11.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "../src/config.h"
#include "../src/hud.h"
#include "../src/stats.h"
#include "installer.h"
#include "metrics_collector.h"

using Microsoft::WRL::ComPtr;
using xrfps::Config;
using xrfps::MetricsCollector;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapchain;
ComPtr<ID3D11RenderTargetView> g_rtv;

void create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                             D3D11_SDK_VERSION, &desc, &g_swapchain, &g_device, nullptr,
                                             &g_context))) {
        return false;
    }
    create_rtv();
    return true;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return true;
    }
    switch (msg) {
    case WM_SIZE:
        if (g_device != nullptr && wp != SIZE_MINIMIZED) {
            g_rtv.Reset();
            g_swapchain->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Synthetic frame history so the preview looks like a live session.
FrameStats make_preview_stats(float target_ms) {
    FrameStats stats{};
    stats.target_ms = target_ms;
    for (int i = 0; i < 320; ++i) {
        float ms = target_ms * (0.94f + 0.05f * std::sin((float)i * 0.11f));
        if (i % 53 == 0) {
            ms += target_ms * 1.1f;
        }
        if (i > 210 && i < 240) {
            ms += target_ms * 0.45f;
        }
        stats.push(ms);
    }
    return stats;
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    // Headless verbs, so installing can be scripted or checked from a shell.
    const std::wstring args = cmdline != nullptr ? cmdline : L"";
    if (args.find(L"--install") != std::wstring::npos ||
        args.find(L"--uninstall") != std::wstring::npos ||
        args.find(L"--status") != std::wstring::npos) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);

        std::string message;
        bool ok = true;
        if (args.find(L"--install") != std::wstring::npos) {
            ok = xrfps::installer::install(&message);
            printf(ok ? "xrFPS: layer installed\n" : "xrFPS: install failed - %s\n", message.c_str());
        } else if (args.find(L"--uninstall") != std::wstring::npos) {
            ok = xrfps::installer::uninstall(&message);
            printf(ok ? "xrFPS: layer uninstalled\n" : "xrFPS: uninstall failed - %s\n",
                   message.c_str());
        } else {
            const auto s = xrfps::installer::query();
            printf("xrFPS: %s%s\n  manifest: %s\n",
                   s.installed ? "installed" : "not installed",
                   s.files_present ? "" : " (layer files missing)", s.manifest_path.c_str());
        }
        return ok ? 0 : 1;
    }

    // Render at the display's real pixel density instead of letting Windows
    // bitmap-stretch the window on a high-DPI monitor.
    using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto set_ctx = (PFN_SetCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext")) {
            set_ctx((HANDLE)-4); // PER_MONITOR_AWARE_V2
        } else {
            SetProcessDPIAware();
        }
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, nullptr, nullptr, nullptr, nullptr,
                   L"xrFPSControlCenter", nullptr};
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const float scale = std::max(1.0f, (float)dpi / 96.0f);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"xrFPS Control Center", WS_OVERLAPPEDWINDOW, 100, 100,
                              (int)(1180 * scale), (int)(820 * scale), nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"xrFPS", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14, 14);
    style.ItemSpacing = ImVec2(10, 8);
    style.ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    Config config{};
    config.load();
    bool dirty = false;
    double dirty_since = 0.0;

    MetricsCollector collector;
    collector.start();
    ULONGLONG last_sample = 0;

    auto status = xrfps::installer::query();
    std::string action_message;

    // Preview texture: the HUD is rasterized by the very same code the layer
    // runs, so what is shown here is what appears in the headset.
    ComPtr<ID3D11Texture2D> preview_tex;
    ComPtr<ID3D11ShaderResourceView> preview_srv;
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = HUD_W;
        td.Height = HUD_H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_device->CreateTexture2D(&td, nullptr, &preview_tex);
        g_device->CreateShaderResourceView(preview_tex.Get(), nullptr, &preview_srv);
    }
    std::vector<uint8_t> preview_pixels(HUD_W * HUD_H * 4);
    float preview_target = 11.1f;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        const ULONGLONG now_tick = GetTickCount64();
        if (now_tick - last_sample > 500) {
            last_sample = now_tick;
            collector.sample();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("xrFPS", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- header: install state -------------------------------------
        ImGui::PushFont(nullptr);
        ImGui::TextUnformatted("xrFPS Control Center");
        ImGui::PopFont();
        ImGui::SameLine(0, 20);
        if (status.installed && status.files_present) {
            ImGui::TextColored(ImVec4(0.35f, 0.82f, 0.52f, 1.0f), "Layer installed");
        } else if (status.installed) {
            ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.22f, 1.0f), "Registered, files missing");
        } else {
            ImGui::TextColored(ImVec4(0.94f, 0.42f, 0.42f, 1.0f), "Not installed");
        }

        ImGui::SameLine(0, 20);
        if (!status.installed) {
            if (ImGui::Button("Install layer", ImVec2(140 * scale, 0))) {
                std::string err;
                action_message = xrfps::installer::install(&err)
                                     ? "Installed. Launch (or relaunch) an OpenXR game to load it."
                                     : "Install failed: " + err;
                status = xrfps::installer::query();
            }
        } else {
            if (ImGui::Button("Uninstall layer", ImVec2(150 * scale, 0))) {
                std::string err;
                action_message = xrfps::installer::uninstall(&err) ? "Uninstalled."
                                                                     : "Uninstall failed: " + err;
                status = xrfps::installer::query();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reinstall / update", ImVec2(180 * scale, 0))) {
                std::string err;
                action_message = xrfps::installer::install(&err) ? "Layer files updated."
                                                                   : "Update failed: " + err;
                status = xrfps::installer::query();
            }
        }
        if (!action_message.empty()) {
            ImGui::TextDisabled("%s", action_message.c_str());
        }
        ImGui::Separator();

        ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.56f, 0), false);

        if (ImGui::Checkbox("Overlay enabled", &config.enabled)) {
            dirty = true;
        }
        help("Master switch. Unlike uninstalling, this keeps the layer loaded and applies "
             "instantly to a game that is already running.");

        if (ImGui::CollapsingHeader("Gesture", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* hands[] = {"Either controller", "Left only", "Right only"};
            if (ImGui::Combo("Controller", &config.hand, hands, 3)) {
                dirty = true;
            }
            if (ImGui::SliderFloat("Flip angle", &config.flip_angle_deg, 60.0f, 175.0f, "%.0f deg")) {
                dirty = true;
            }
            help("How far past upright the controller must rotate. Lower is easier to trigger; "
                 "higher avoids showing the HUD during normal play.");
            if (ImGui::SliderFloat("Show delay", &config.show_delay_sec, 0.0f, 1.5f, "%.2f s")) {
                dirty = true;
            }
            if (ImGui::SliderFloat("Hide delay", &config.hide_delay_sec, 0.0f, 3.0f, "%.2f s")) {
                dirty = true;
            }
            if (ImGui::Checkbox("F10 toggles a head-locked HUD", &config.f10_toggle)) {
                dirty = true;
            }
        }

        if (ImGui::CollapsingHeader("Placement & look", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::SliderFloat("Panel width", &config.panel_width_m, 0.10f, 0.80f, "%.2f m")) {
                dirty = true;
            }
            if (ImGui::SliderFloat("Distance from controller", &config.panel_offset_m, 0.0f, 0.40f,
                                   "%.2f m")) {
                dirty = true;
            }
            if (ImGui::SliderFloat("Roll", &config.panel_roll_deg, -90.0f, 90.0f, "%.0f deg")) {
                dirty = true;
            }
            if (ImGui::SliderFloat("Opacity", &config.opacity, 0.15f, 1.0f, "%.2f")) {
                dirty = true;
            }
            const char* themes[] = {"Dark", "Light", "High contrast"};
            if (ImGui::Combo("Theme", &config.theme, themes, 3)) {
                dirty = true;
            }
            if (ImGui::SliderFloat("HUD refresh", &config.hud_refresh_hz, 1.0f, 20.0f, "%.1f Hz")) {
                dirty = true;
            }
            help("How often the panel's pixels are redrawn. The frametime graph samples every frame "
                 "regardless; lower values leave the game's render thread completely alone.");
        }

        if (ImGui::CollapsingHeader("Readouts", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Columns(2, nullptr, false);
            const auto row = [&](const char* label, bool* value, const char* tip = nullptr) {
                if (ImGui::Checkbox(label, value)) {
                    dirty = true;
                }
                if (tip != nullptr) {
                    help(tip);
                }
                ImGui::NextColumn();
            };
            row("FPS", &config.show_fps);
            row("Frametime", &config.show_frametime);
            row("Target", &config.show_target);
            row("On-target %", &config.show_on_target,
                "Share of recent frames that hit the display's period — the honest 'is it actually "
                "smooth' number.");
            row("1% low", &config.show_percentiles);
            row("Frametime graph", &config.show_graph);
            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::TextDisabled("System metrics (from this app)");
            ImGui::Columns(2, nullptr, false);
            row("CPU usage", &config.show_cpu_usage);
            row("GPU usage", &config.show_gpu_usage);
            row("GPU usage (app)", &config.show_gpu_app_usage,
                "The VR application's own share of the GPU, measured per process.");
            row("VRAM", &config.show_vram);
            row("GPU clock", &config.show_gpu_clock);
            row("GPU temp", &config.show_gpu_temp);
            row("CPU temp", &config.show_cpu_temp,
                "Requires a source that exposes CPU temperature; hidden when unavailable.");
            row("System RAM", &config.show_ram);
            ImGui::Columns(1);

            const auto& m = collector.latest();
            ImGui::Spacing();
            ImGui::TextDisabled("Collector: %s", collector.status().c_str());
            if (m.app_pid != 0) {
                ImGui::TextDisabled("VR app: %s (pid %u)", m.app_name, m.app_pid);
            } else {
                ImGui::TextDisabled("VR app: none detected");
            }
            char app_gpu[16];
            if (m.gpu_app_usage_pct >= 0.0f) {
                snprintf(app_gpu, sizeof(app_gpu), "%.0f%%", m.gpu_app_usage_pct);
            } else {
                snprintf(app_gpu, sizeof(app_gpu), "n/a");
            }
            ImGui::TextDisabled("CPU %.0f%%   GPU %.0f%%   app %s   VRAM %.1f/%.1f GB   %.0f C",
                                m.cpu_usage_pct, m.gpu_usage_pct, app_gpu, m.vram_used_mb / 1024.0f,
                                m.vram_total_mb / 1024.0f, m.gpu_temp_c);
        }

        ImGui::Spacing();
        if (ImGui::Button("Restore defaults", ImVec2(170 * scale, 0))) {
            config = Config{};
            dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Settings apply live - no need to restart the game.");

        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), false);

        ImGui::TextUnformatted("Preview");
        ImGui::SameLine();
        ImGui::TextDisabled("(rendered by the layer's own HUD code)");

        ImGui::SetNextItemWidth(160 * scale);
        if (ImGui::SliderFloat("Preview refresh rate", &preview_target, 4.0f, 20.0f, "%.1f ms")) {
        }
        help("Pretend display period, so the preview colors match what your headset will show.");

        {
            const FrameStats preview_stats = make_preview_stats(preview_target);
            const xrfps::SharedMetrics& live = collector.latest();
            hud_render(preview_pixels.data(), preview_stats, config,
                       collector.running() ? &live : nullptr, /*bgra=*/false);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_context->Map(preview_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                for (int y = 0; y < HUD_H; ++y) {
                    memcpy((uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
                           preview_pixels.data() + (size_t)y * HUD_W * 4, HUD_W * 4);
                }
                g_context->Unmap(preview_tex.Get(), 0);
            }
        }

        // Checkerboard behind the panel so transparency is visible.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float w = std::min(ImGui::GetContentRegionAvail().x, 512.0f);
        const float h = w * (float)HUD_H / (float)HUD_W;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int y = 0; y * 16 < (int)h; ++y) {
            for (int x = 0; x * 16 < (int)w; ++x) {
                const ImU32 c = ((x + y) & 1) ? IM_COL32(58, 58, 64, 255) : IM_COL32(44, 44, 50, 255);
                draw->AddRectFilled(ImVec2(origin.x + x * 16.0f, origin.y + y * 16.0f),
                                    ImVec2(origin.x + std::min((x + 1) * 16.0f, w),
                                           origin.y + std::min((y + 1) * 16.0f, h)),
                                    c);
            }
        }
        ImGui::Image((ImTextureID)preview_srv.Get(), ImVec2(w, h));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Config file");
        ImGui::TextWrapped("%s", Config::path().c_str());
        if (ImGui::Button("Open config folder")) {
            const std::string dir = status.install_dir;
            ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open log")) {
            const std::string log = status.install_dir + "\\xrfps.log";
            ShellExecuteA(nullptr, "open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("In the headset: hold a controller upside down to summon the HUD.");

        ImGui::EndChild();
        ImGui::End();

        // Coalesce slider drags into one write rather than hammering the file
        // the layer is polling.
        if (dirty) {
            if (dirty_since == 0.0) {
                dirty_since = ImGui::GetTime();
            } else if (ImGui::GetTime() - dirty_since > 0.35) {
                config.save();
                dirty = false;
                dirty_since = 0.0;
            }
        }

        ImGui::Render();
        const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        ID3D11RenderTargetView* rtv = g_rtv.Get();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        g_context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
    }

    if (dirty) {
        config.save();
    }
    collector.stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
