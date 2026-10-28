#include "overlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include "hud.h"
#include "frame_timing.h"
#include "log.h"

namespace xrfps {
namespace {

constexpr float kQuadAspect = (float)HUD_H / (float)HUD_W;

double qpc_ms() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / freq;
}

struct V3 {
    float x, y, z;
};

constexpr float kPi = 3.14159265358979323846f;

enum class PlacementKind { Controller, View, Follow, Fixed, Hidden };

struct PlacementPreset {
    PlacementKind kind;
    // 0 = left, 1 = right, -1 = use the gesture-hand selection.
    int hand;
    float pitch_deg;
    float yaw_deg;
    float roll_deg;
    V3 offset;
};

// glm convention: (a*b) applies b first, then a.
XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

V3 rotate(const XrQuaternionf& q, V3 v) {
    // v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
    const V3 u{q.x, q.y, q.z};
    const V3 c1{u.y * v.z - u.z * v.y + q.w * v.x,
                u.z * v.x - u.x * v.z + q.w * v.y,
                u.x * v.y - u.y * v.x + q.w * v.z};
    return {v.x + 2.0f * (u.y * c1.z - u.z * c1.y),
            v.y + 2.0f * (u.z * c1.x - u.x * c1.z),
            v.z + 2.0f * (u.x * c1.y - u.y * c1.x)};
}

XrQuaternionf axis_angle(float x, float y, float z, float radians) {
    const float half = radians * 0.5f;
    const float sine = std::sin(half);
    return {x * sine, y * sine, z * sine, std::cos(half)};
}

// Preset rotations compose as yaw * pitch * roll in OpenXR's right-handed
// X-right/Y-up/-Z-forward tracking convention.
XrQuaternionf preset_orientation(float pitch_deg, float yaw_deg, float roll_deg) {
    const XrQuaternionf yaw = axis_angle(0.0f, 1.0f, 0.0f, yaw_deg * kPi / 180.0f);
    const XrQuaternionf pitch = axis_angle(1.0f, 0.0f, 0.0f, pitch_deg * kPi / 180.0f);
    const XrQuaternionf roll = axis_angle(0.0f, 0.0f, 1.0f, roll_deg * kPi / 180.0f);
    return qmul(qmul(yaw, pitch), roll);
}

PlacementPreset controller_preset(int value) {
    // Slot zero is xrFPS's original adaptive underside placement, retained as
    // the default; 1..8 are fixed mounts around either controller, 9..10 are
    // anchored to the headset instead.
    switch (std::clamp(value, 0, 10)) {
    case 1: return {PlacementKind::Controller, 0, -30, 0, 0, {0.10f, -0.05f, 0.22f}};
    case 2: return {PlacementKind::Controller, 0, -45, 0, 0, {0.10f, -0.05f, 0.23f}};
    case 3: return {PlacementKind::Controller, 0, 0, -90, -180, {-0.05f, 0.0f, 0.20f}};
    case 4: return {PlacementKind::Controller, 0, 90, 0, 180, {0.0f, -0.10f, 0.0f}};
    case 5: return {PlacementKind::Controller, 1, -30, 0, 0, {-0.10f, -0.05f, 0.22f}};
    case 6: return {PlacementKind::Controller, 1, -45, 0, 0, {-0.10f, -0.05f, 0.23f}};
    case 7: return {PlacementKind::Controller, 1, 0, 90, 180, {0.05f, 0.0f, 0.20f}};
    case 8: return {PlacementKind::Controller, 1, 90, 0, 180, {0.0f, -0.10f, 0.0f}};
    case 9: return {PlacementKind::View, -1, -15, 0, 0, {0.0f, -0.20f, -0.45f}};
    case 10: return {PlacementKind::Follow, -1, -60, 0, 0, {0.0f, -0.40f, -0.30f}};
    default: return {PlacementKind::Controller, -1, 0, 0, 0, {0, 0, 0}};
    }
}

PlacementPreset fallback_preset(int value) {
    // Placements that need no controller pose: follow panels, head locked,
    // and fixed room anchors.
    switch (std::clamp(value, 0, 7)) {
    case 1: return {PlacementKind::Follow, -1, -30, 30, 0, {-0.25f, -0.30f, -0.40f}};
    case 2: return {PlacementKind::Follow, -1, -30, -30, 0, {0.25f, -0.30f, -0.40f}};
    case 3: return {PlacementKind::View, -1, -15, 0, 0, {0.0f, -0.20f, -0.45f}};
    case 4: return {PlacementKind::Fixed, -1, -60, 0, 0, {0.0f, -0.40f, -0.30f}};
    case 5: return {PlacementKind::Fixed, -1, -30, 30, 0, {-0.25f, -0.30f, -0.40f}};
    case 6: return {PlacementKind::Fixed, -1, -30, -30, 0, {0.25f, -0.30f, -0.40f}};
    case 7: return {PlacementKind::Hidden, -1, 0, 0, 0, {0, 0, 0}};
    default: return {PlacementKind::Follow, -1, -60, 0, 0, {0.0f, -0.40f, -0.30f}};
    }
}

XrPosef local_pose(const PlacementPreset& preset, float extra_roll_deg) {
    XrPosef pose{};
    pose.orientation = preset_orientation(preset.pitch_deg, preset.yaw_deg,
                                          preset.roll_deg + extra_roll_deg);
    pose.position = {preset.offset.x, preset.offset.y, preset.offset.z};
    return pose;
}

XrPosef compose_pose(const XrPosef& base, const XrPosef& local) {
    const V3 translated = rotate(base.orientation,
                                 {local.position.x, local.position.y, local.position.z});
    XrPosef result{};
    result.orientation = qmul(base.orientation, local.orientation);
    result.position = {base.position.x + translated.x, base.position.y + translated.y,
                       base.position.z + translated.z};
    return result;
}

float wrap_angle(float radians) {
    while (radians > kPi) radians -= 2.0f * kPi;
    while (radians < -kPi) radians += 2.0f * kPi;
    return radians;
}

float yaw_from_quaternion(const XrQuaternionf& q) {
    return std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                      1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

// Undoes a partially built swapchain. Leaving the handle set while its upload
// objects are missing would make the next call succeed at the top and then
// dereference a null fence.
void discard_swapchain(Session* s) {
    if (s->swapchain != XR_NULL_HANDLE) {
        s->fns->destroySwapchain(s->swapchain);
        s->swapchain = XR_NULL_HANDLE;
    }
    s->images_d3d11.clear();
    s->images_d3d12.clear();
    s->d12_allocator.Reset();
    s->d12_list.Reset();
    s->d12_fence.Reset();
    s->d12_upload.Reset();
    s->d12_upload_ptr = nullptr;
    s->d12_fence_value = 0;
    s->ever_uploaded = false;
}

bool ensure_swapchain(Session* s) {
    if (s->swapchain != XR_NULL_HANDLE) {
        return true;
    }
    if (s->gfx == Gfx::None) {
        return false;
    }
    // Give up after a few attempts rather than retrying every single frame for
    // the life of the session.
    if (s->swapchain_attempts >= 3) {
        return false;
    }
    ++s->swapchain_attempts;

    // Formats are DXGI enums for both D3D11 and D3D12 sessions.
    uint32_t count = 0;
    s->fns->enumerateSwapchainFormats(s->session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    s->fns->enumerateSwapchainFormats(s->session, count, &count, formats.data());

    constexpr int64_t kPreferred[] = {
        91 /* DXGI_FORMAT_B8G8R8A8_UNORM_SRGB */, 29 /* R8G8B8A8_UNORM_SRGB */,
        87 /* B8G8R8A8_UNORM */, 28 /* R8G8B8A8_UNORM */};
    int64_t chosen = 0;
    for (const int64_t want : kPreferred) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
            chosen = want;
            break;
        }
    }
    if (chosen == 0) {
        flog("no usable swapchain format (got %u formats)", count);
        return false;
    }

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = HUD_W;
    info.height = HUD_H;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrResult r = s->fns->createSwapchain(s->session, &info, &s->swapchain);
    if (XR_FAILED(r)) {
        flog("xrCreateSwapchain failed: %d", r);
        s->swapchain = XR_NULL_HANDLE;
        return false;
    }
    s->swapchain_format = chosen;
    s->swapchain_bgra = chosen == 91 || chosen == 87;

    uint32_t image_count = 0;
    s->fns->enumerateSwapchainImages(s->swapchain, 0, &image_count, nullptr);
    if (image_count == 0) {
        flog("swapchain reported no images");
        discard_swapchain(s);
        return false;
    }
    if (s->gfx == Gfx::D3D11) {
        s->images_d3d11.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        s->fns->enumerateSwapchainImages(s->swapchain, image_count, &image_count,
                                         (XrSwapchainImageBaseHeader*)s->images_d3d11.data());
    } else {
        s->images_d3d12.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        s->fns->enumerateSwapchainImages(s->swapchain, image_count, &image_count,
                                         (XrSwapchainImageBaseHeader*)s->images_d3d12.data());

        if (FAILED(s->d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&s->d12_allocator))) ||
            FAILED(s->d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->d12_allocator.Get(),
                                                    nullptr, IID_PPV_ARGS(&s->d12_list))) ||
            FAILED(s->d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->d12_fence)))) {
            flog("d3d12 upload objects failed");
            discard_swapchain(s);
            return false;
        }
        s->d12_list->Close();

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = HUD_W * HUD_H * 4;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s->d12_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&s->d12_upload)))) {
            flog("d3d12 upload buffer failed");
            discard_swapchain(s);
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(s->d12_upload->Map(0, nullptr, &mapped)) || mapped == nullptr) {
            flog("d3d12 upload buffer could not be mapped");
            discard_swapchain(s);
            return false;
        }
        s->d12_upload_ptr = (uint8_t*)mapped;
    }

    s->pixels.resize(HUD_W * HUD_H * 4);
    flog("swapchain ready: format %lld, %u images, %s", (long long)chosen, image_count,
         s->gfx == Gfx::D3D11 ? "d3d11" : "d3d12");
    return true;
}

// Renders the HUD and moves it into the current swapchain image. Returns false
// if nothing has ever been uploaded (the quad must not be shown yet).
bool update_content(Session* s) {
    // Refresh the pixels at ~10 Hz, not every frame: the acquire/upload/release
    // round-trip runs on the app's render thread inside xrEndFrame, and some
    // runtimes block in xrWaitSwapchainImage — repainting per frame adds real
    // latency to the host app. Between refreshes the quad keeps compositing
    // the last released image at zero cost.
    const double kHudRefreshIntervalMs =
        1000.0 / std::clamp(s->config.hud_refresh_hz, 1.0f, 30.0f);
    const double now = qpc_ms();
    if (s->ever_uploaded && now - s->last_upload_qpc_ms < kHudRefreshIntervalMs) {
        return true;
    }

    if (s->gfx == Gfx::D3D12 && s->d12_fence->GetCompletedValue() < s->d12_fence_value) {
        return s->ever_uploaded; // previous copy still in flight; reuse last released image
    }

    hud_render(s->pixels.data(), s->stats, s->config, s->metrics.get(), s->swapchain_bgra);
    const double t_rastered = qpc_ms();

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(s->fns->acquireSwapchainImage(s->swapchain, &acquire, &index))) {
        return s->ever_uploaded;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000; // 100 ms
    if (XR_FAILED(s->fns->waitSwapchainImage(s->swapchain, &wait))) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        s->fns->releaseSwapchainImage(s->swapchain, &release);
        return s->ever_uploaded;
    }
    const double t_waited = qpc_ms();

    if (s->gfx == Gfx::D3D11) {
        s->d11_context->UpdateSubresource(s->images_d3d11[index].texture, 0, nullptr, s->pixels.data(),
                                          HUD_W * 4, 0);
    } else {
        std::memcpy(s->d12_upload_ptr, s->pixels.data(), s->pixels.size());

        s->d12_allocator->Reset();
        s->d12_list->Reset(s->d12_allocator.Get(), nullptr);

        ID3D12Resource* tex = s->images_d3d12[index].texture;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s->d12_list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = s->d12_upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)s->swapchain_format;
        src.PlacedFootprint.Footprint.Width = HUD_W;
        src.PlacedFootprint.Footprint.Height = HUD_H;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = HUD_W * 4; // 2048, already 256-aligned
        s->d12_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        s->d12_list->ResourceBarrier(1, &barrier);
        s->d12_list->Close();

        ID3D12CommandList* lists[] = {s->d12_list.Get()};
        s->d12_queue->ExecuteCommandLists(1, lists);
        s->d12_queue->Signal(s->d12_fence.Get(), ++s->d12_fence_value);
    }

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    s->fns->releaseSwapchainImage(s->swapchain, &release);
    s->ever_uploaded = true;
    s->hud_refreshed_this_frame = true;
    s->last_upload_qpc_ms = now;
    // Diagnostics: a refresh that visibly stalls the host frame is a bug —
    // report where the time went (throttled to one line per 2 s).
    const double total = qpc_ms() - now;
    static double last_slow_log = 0.0;
    if (total > 2.0 && now - last_slow_log > 2000.0) {
        last_slow_log = now;
        flog("slow hud refresh: %.2f ms (raster %.2f, acquire+wait %.2f, upload+release %.2f)", total,
             t_rastered - now, t_waited - t_rastered, qpc_ms() - t_waited);
    }
    return true;
}

bool hand_allowed(const Session* s, const PlacementPreset& preset, int hand) {
    if (preset.hand >= 0) {
        return hand == preset.hand;
    }
    return (s->config.hand == 0) || (s->config.hand == 1 && hand == 0) ||
           (s->config.hand == 2 && hand == 1);
}

bool selected_controller_available(const Session* s, const PlacementPreset& preset,
                                   const bool grip_valid[2]) {
    if (preset.hand >= 0) {
        return grip_valid[preset.hand];
    }
    if (s->config.hand == 1) return grip_valid[0];
    if (s->config.hand == 2) return grip_valid[1];
    return grip_valid[0] || grip_valid[1];
}

bool set_follow_anchor(Session* s, const PlacementPreset& preset, XrTime display_time, double now) {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(s->fns->locateSpace(s->view_space, s->ref_space, display_time, &head))) {
        return false;
    }
    constexpr XrSpaceLocationFlags kNeeded =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((head.locationFlags & kNeeded) != kNeeded) {
        return false;
    }

    const float target_yaw = yaw_from_quaternion(head.pose.orientation);
    const float dx = head.pose.position.x - s->follow_position.x;
    const float dy = head.pose.position.y - s->follow_position.y;
    const float dz = head.pose.position.z - s->follow_position.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double elapsed_ms = now - s->last_follow_update_ms;
    const float dt = (float)std::clamp(elapsed_ms / 1000.0, 0.0, 0.1);

    if (!s->follow_valid || elapsed_ms > 1000.0) {
        s->follow_position = head.pose.position;
        s->follow_yaw_rad = target_yaw;
        s->follow_valid = true;
    } else {
        float position_alpha = 1.0f - std::exp(-4.0f * dt);
        if (distance > 0.5f) position_alpha = std::max(position_alpha, 0.35f);
        if (distance > 1.5f) position_alpha = 1.0f;
        s->follow_position.x += dx * position_alpha;
        s->follow_position.y += dy * position_alpha;
        s->follow_position.z += dz * position_alpha;

        const float yaw_delta = wrap_angle(target_yaw - s->follow_yaw_rad);
        const float yaw_degrees = std::fabs(yaw_delta) * 180.0f / kPi;
        float yaw_alpha = 1.0f - std::exp(-2.5f * dt);
        if (yaw_degrees > 70.0f) yaw_alpha = std::max(yaw_alpha, 0.20f);
        if (yaw_degrees > 110.0f) yaw_alpha = 1.0f;
        s->follow_yaw_rad = wrap_angle(s->follow_yaw_rad + yaw_delta * yaw_alpha);
    }
    s->last_follow_update_ms = now;

    XrPosef smoothed_head{};
    smoothed_head.orientation = axis_angle(0.0f, 1.0f, 0.0f, s->follow_yaw_rad);
    smoothed_head.position = s->follow_position;
    s->anchor_pose = compose_pose(smoothed_head, local_pose(preset, s->config.panel_roll_deg));
    s->anchor_space = s->ref_space;
    s->anchor_valid = true;
    return true;
}

bool set_non_controller_anchor(Session* s, const PlacementPreset& preset, XrTime display_time,
                               double now) {
    switch (preset.kind) {
    case PlacementKind::View:
        s->anchor_pose = local_pose(preset, s->config.panel_roll_deg);
        s->anchor_space = s->view_space;
        s->anchor_valid = true;
        return true;
    case PlacementKind::Follow:
        return set_follow_anchor(s, preset, display_time, now);
    case PlacementKind::Fixed:
        s->anchor_pose = local_pose(preset, s->config.panel_roll_deg);
        s->anchor_space = s->ref_space;
        s->anchor_valid = true;
        return true;
    case PlacementKind::Hidden:
    case PlacementKind::Controller:
    default:
        return false;
    }
}

bool set_controller_anchor(Session* s, const PlacementPreset& preset, const XrPosef grip_pose[2],
                           const bool grip_valid[2], XrTime display_time, double now) {
    if (preset.kind != PlacementKind::Controller) {
        return set_non_controller_anchor(s, preset, display_time, now);
    }

    int hand = preset.hand;
    if (hand < 0) {
        hand = s->anchor_hand;
        if (hand < 0 || !grip_valid[hand] || !hand_allowed(s, preset, hand)) {
            hand = grip_valid[0] && hand_allowed(s, preset, 0) ? 0
                   : (grip_valid[1] && hand_allowed(s, preset, 1) ? 1 : -1);
        }
    }
    if (hand < 0 || !grip_valid[hand]) {
        return false;
    }
    s->anchor_hand = hand;

    if (s->config.controller_placement == 0) {
        // Original xrFPS adaptive underside position. Keeping it as its own
        // preset means existing configs and the distance slider behave exactly
        // as before.
        const float roll = s->config.panel_roll_deg * kPi / 180.0f;
        const XrQuaternionf flip_z{0.0f, 0.0f, 1.0f, 0.0f};
        const XrQuaternionf user_roll = axis_angle(0.0f, 0.0f, 1.0f, roll);
        s->anchor_pose.orientation =
            qmul(qmul(grip_pose[hand].orientation, flip_z), user_roll);
        const V3 off = rotate(grip_pose[hand].orientation,
                              {0.0f, -s->config.panel_offset_m, 0.0f});
        s->anchor_pose.position = {grip_pose[hand].position.x + off.x,
                                   grip_pose[hand].position.y + off.y,
                                   grip_pose[hand].position.z + off.z};
    } else {
        s->anchor_pose =
            compose_pose(grip_pose[hand], local_pose(preset, s->config.panel_roll_deg));
    }
    s->anchor_space = s->ref_space;
    s->anchor_valid = true;
    return true;
}

void update_gesture(Session* s, XrTime display_time) {
    const double now = qpc_ms();

    // Optional test pose injection for the left hand (see overlay.h).
    if (!s->test_pose_env_checked) {
        s->test_pose_env_checked = true;
        char value[8]{};
        s->test_pose_env =
            GetEnvironmentVariableA("XRFPS_TEST_POSE", value, sizeof(value)) > 0 && value[0] == '1';
        if (s->test_pose_env) {
            flog("test pose injection enabled; waiting for shared memory");
        }
    }
    if (s->test_pose_env && s->test_pose_view == nullptr) {
        s->test_pose_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\xrFPS_TestPose");
        if (s->test_pose_mapping != nullptr) {
            s->test_pose_view =
                (const volatile float*)MapViewOfFile(s->test_pose_mapping, FILE_MAP_READ, 0, 0, 0);
            flog("test pose shared memory connected");
        }
    }

    XrPosef grip_pose[2]{};
    bool grip_valid[2]{false, false};
    for (int hand = 0; hand < 2; ++hand) {
        if (s->grip_space[hand] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(s->fns->locateSpace(s->grip_space[hand], s->ref_space, display_time, &loc))) {
            continue;
        }
        // TRACKED as well as VALID: runtimes keep VALID set on a controller
        // they have lost, serving the last known pose. Accepting that would
        // freeze the panel in mid-air instead of handing over to the fallback.
        constexpr XrSpaceLocationFlags kNeeded =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if ((loc.locationFlags & kNeeded) != kNeeded) {
            continue;
        }
        grip_pose[hand] = loc.pose;
        grip_valid[hand] = true;
    }

    if (s->test_pose_view != nullptr && s->test_pose_view[0] == 1.0f) {
        grip_pose[0].position = {s->test_pose_view[1], s->test_pose_view[2], s->test_pose_view[3]};
        grip_pose[0].orientation = {s->test_pose_view[4], s->test_pose_view[5], s->test_pose_view[6],
                                    s->test_pose_view[7]};
        grip_valid[0] = true;
    }

    const PlacementPreset placement = controller_preset(s->config.controller_placement);
    const bool available = selected_controller_available(s, placement, grip_valid);
    if (!s->controller_availability_known || available != s->controller_available) {
        s->controller_availability_known = true;
        s->controller_available = available;
        flog("selected controller: %s; %s placement active", available ? "tracked" : "unavailable",
             available ? "controller" : "fallback");
    }
    s->fallback_active = !available;

    // "Upside down" is the grip's up-vector tipped past the configured angle;
    // cos() converts the user-facing degrees into the comparison directly.
    bool flipped = false;
    const float flip_cos =
        std::cos(std::clamp(s->config.flip_angle_deg, 45.0f, 179.0f) * kPi / 180.0f);
    for (int hand = 0; hand < 2; ++hand) {
        if (!grip_valid[hand] || !hand_allowed(s, placement, hand)) {
            continue;
        }
        const V3 up = rotate(grip_pose[hand].orientation, {0.0f, 1.0f, 0.0f});
        if (!flipped && up.y < flip_cos) {
            flipped = true;
            s->anchor_hand = hand;
        }
    }

    if (flipped) {
        s->unflip_since_ms = 0.0;
        if (s->flip_since_ms == 0.0) {
            s->flip_since_ms = now;
        } else if (!s->visible && now - s->flip_since_ms > s->config.show_delay_sec * 1000.0) {
            s->visible = true;
            flog("gesture: shown");
        }
    } else {
        s->flip_since_ms = 0.0;
        if (s->visible) {
            if (s->unflip_since_ms == 0.0) {
                s->unflip_since_ms = now;
            } else if (now - s->unflip_since_ms > s->config.hide_delay_sec * 1000.0) {
                s->visible = false;
                flog("gesture: hidden");
            }
        }
    }

    // F10 forces the HUD visible when no gesture can (unbound controllers, a
    // seated game). GetAsyncKeyState is system-wide, so require this process to
    // own the foreground window -- otherwise F10 pressed in any other
    // application would silently pin an overlay inside the game.
    if (s->config.f10_toggle) {
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10) {
            DWORD foreground_pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
            f10 = foreground_pid == GetCurrentProcessId();
        }
        if (f10 && !s->f10_was_down) {
            s->force_show = !s->force_show;
            flog("F10 force-show: %d", (int)s->force_show);
        }
        s->f10_was_down = f10;
    }

    // Resolve the overlay transform after visibility and tracking state. A
    // missing selected controller activates its independent fallback profile
    // immediately. The fallback is intentionally persistent:
    // otherwise there would be no controller gesture available to summon it.
    s->anchor_valid = false;
    s->anchor_space = XR_NULL_HANDLE;
    if (s->fallback_active) {
        set_non_controller_anchor(s, fallback_preset(s->config.fallback_placement), display_time,
                                  now);
    } else if (s->visible || placement.kind == PlacementKind::View ||
               placement.kind == PlacementKind::Follow) {
        // Controller-attached presets retain xrFPS's flip-to-show behavior.
        // Headset and follow presets are persistent instead: they stay up
        // while the selected controller is available, so switching away from
        // the controller-off fallback cannot make the overlay disappear.
        set_controller_anchor(s, placement, grip_pose, grip_valid, display_time, now);
    }
}

bool contains_case_insensitive(const char* text, const char* needle) {
    if (text == nullptr || needle == nullptr) {
        return false;
    }
    std::string haystack{text};
    std::string wanted{needle};
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return haystack.find(wanted) != std::string::npos;
}

const char* timing_source_name(const Session* s) {
    switch (s->stats.source) {
    case TimingBackend::SteamVR:
        if (s->config.timing_source == 0 && s->meta_cpu_path != XR_NULL_PATH &&
            s->meta_gpu_path != XR_NULL_PATH) {
            return "SteamVR + Meta CPU/GPU";
        }
        if (s->config.timing_source == 0 && s->meta_gpu_path != XR_NULL_PATH) {
            return "SteamVR + Meta GPU";
        }
        if (s->config.timing_source == 0 && s->meta_cpu_path != XR_NULL_PATH) {
            return "SteamVR + Meta CPU";
        }
        return "SteamVR compositor";
    case TimingBackend::Meta:
        return "Meta metrics + OpenXR FPS";
    case TimingBackend::OpenXR:
    default:
        return s->config.timing_source == 0 || s->config.timing_source == 3
                   ? "OpenXR submissions"
                   : "OpenXR fallback";
    }
}

void initialize_meta_timing(Session* s, XrInstance instance) {
    if (s->fns->setPerformanceMetricsStateMETA == nullptr ||
        s->fns->enumeratePerformanceMetricsCounterPathsMETA == nullptr ||
        s->fns->queryPerformanceMetricsCounterMETA == nullptr) {
        return;
    }

    XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
    state.enabled = XR_TRUE;
    const XrResult enabled = s->fns->setPerformanceMetricsStateMETA(s->session, &state);
    if (XR_FAILED(enabled)) {
        flog("Meta performance metrics could not be enabled (%d)", enabled);
        return;
    }

    s->fns->stringToPath(instance, "/perfmetrics_meta/app/cpu_frametime", &s->meta_cpu_path);
    s->fns->stringToPath(instance, "/perfmetrics_meta/app/gpu_frametime", &s->meta_gpu_path);

    uint32_t count = 0;
    if (XR_FAILED(s->fns->enumeratePerformanceMetricsCounterPathsMETA(instance, 0, &count, nullptr)) ||
        count == 0) {
        return;
    }
    std::vector<XrPath> paths(count);
    if (XR_FAILED(s->fns->enumeratePerformanceMetricsCounterPathsMETA(
            instance, count, &count, paths.data()))) {
        return;
    }
    const bool has_cpu = std::find(paths.begin(), paths.end(), s->meta_cpu_path) != paths.end();
    const bool has_gpu = std::find(paths.begin(), paths.end(), s->meta_gpu_path) != paths.end();
    s->meta_timing_available = has_cpu || has_gpu;
    if (!has_cpu) {
        s->meta_cpu_path = XR_NULL_PATH;
    }
    if (!has_gpu) {
        s->meta_gpu_path = XR_NULL_PATH;
    }
    if (s->meta_timing_available) {
        flog("Meta performance metrics ready: cpu=%d gpu=%d", (int)has_cpu, (int)has_gpu);
    }
}

float query_meta_counter(Session* s, XrPath path) {
    if (path == XR_NULL_PATH) {
        return -1.0f;
    }
    XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    if (XR_FAILED(s->fns->queryPerformanceMetricsCounterMETA(s->session, path, &counter)) ||
        counter.counterUnit != XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META) {
        return -1.0f;
    }
    if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) != 0) {
        return counter.floatValue > 0.0f ? counter.floatValue : -1.0f;
    }
    if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) != 0) {
        return counter.uintValue > 0 ? (float)counter.uintValue : -1.0f;
    }
    return -1.0f;
}

void poll_timing_source(Session* s, double now) {
    // Runtime calls can involve IPC. 20 Hz is fast enough for a 6.7 Hz HUD,
    // while SteamVR returns every intervening frame so its graph stays dense.
    if (now - s->last_timing_poll_qpc_ms < 50.0) {
        return;
    }
    s->last_timing_poll_qpc_ms = now;

    const int requested = std::clamp(s->config.timing_source, 0, 3);
    const bool auto_mode = requested == 0;
    const bool want_meta = requested == 2 || (auto_mode && s->meta_timing_available);
    const bool want_steamvr = s->runtime_is_steamvr && (requested == 1 || auto_mode);

    if (want_meta && s->meta_timing_available) {
        s->latest_meta_cpu_ms = query_meta_counter(s, s->meta_cpu_path);
        s->latest_meta_gpu_ms = query_meta_counter(s, s->meta_gpu_path);
    }

    std::array<RuntimeTimingSample, 128> steam_samples{};
    int steam_count = 0;
    if (want_steamvr) {
        steam_count = s->steamvr_timing.collect(steam_samples.data(), (int)steam_samples.size());
    }

    TimingBackend selected = TimingBackend::OpenXR;
    if (want_steamvr && s->steamvr_timing.ready()) {
        selected = TimingBackend::SteamVR;
    } else if (want_meta && s->meta_timing_available) {
        selected = TimingBackend::Meta;
    }

    if (s->last_timing_source_config != requested || s->stats.source != selected) {
        s->last_timing_source_config = requested;
        s->stats.clear(selected);
        flog("timing source: requested=%d active=%s", requested, timing_source_name(s));
    }

    if (selected == TimingBackend::SteamVR) {
        for (int i = 0; i < steam_count; ++i) {
            const float cpu = auto_mode && s->latest_meta_cpu_ms > 0.0f
                                  ? s->latest_meta_cpu_ms
                                  : steam_samples[i].cpu_ms;
            const float gpu = auto_mode && s->latest_meta_gpu_ms > 0.0f
                                  ? s->latest_meta_gpu_ms
                                  : steam_samples[i].gpu_ms;
            s->stats.push(steam_samples[i].frame_ms, cpu, gpu);
        }
    }
}

} // namespace

Session* session_create(XrInstance instance, XrSession session, const XrSessionCreateInfo* create_info,
                         const NextFns* fns, const char* app_name, const char* runtime_name) {
    auto* s = new Session();
    s->fns = fns;
    s->session = session;
    s->session_info.start(app_name);
    s->runtime_is_steamvr = contains_case_insensitive(runtime_name, "steamvr") ||
                            contains_case_insensitive(runtime_name, "valve");

    for (const auto* entry = (const XrBaseInStructure*)create_info->next; entry != nullptr;
         entry = entry->next) {
        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* b = (const XrGraphicsBindingD3D11KHR*)entry;
            s->gfx = Gfx::D3D11;
            s->d11_device = b->device;
            b->device->GetImmediateContext(&s->d11_context);
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* b = (const XrGraphicsBindingD3D12KHR*)entry;
            s->gfx = Gfx::D3D12;
            s->d12_device = b->device;
            s->d12_queue = b->queue;
        }
    }

    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    fns->createReferenceSpace(session, &space, &s->ref_space);
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    fns->createReferenceSpace(session, &space, &s->view_space);

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    if (create_info->systemId != XR_NULL_SYSTEM_ID &&
        XR_SUCCEEDED(fns->getSystemProperties(instance, create_info->systemId, &props))) {
        s->max_layers = props.graphicsProperties.maxLayerCount;
    }

    s->config.load(); // absent file keeps the defaults
    s->config_stamp = Config::stamp();
    initialize_meta_timing(s, instance);

    flog("session created: runtime=%s gfx=%s maxLayers=%u", runtime_name != nullptr ? runtime_name : "",
         s->gfx == Gfx::D3D11 ? "d3d11" : (s->gfx == Gfx::D3D12 ? "d3d12" : "unsupported"),
         s->max_layers);
    return s;
}

void session_destroy(Session* s) {
    if (s == nullptr) {
        return;
    }
    if (s->gfx == Gfx::D3D12 && s->d12_fence) {
        // Give any in-flight copy a moment; the queue belongs to the app.
        for (int i = 0; i < 100 && s->d12_fence->GetCompletedValue() < s->d12_fence_value; ++i) {
            Sleep(1);
        }
    }
    if (s->swapchain != XR_NULL_HANDLE) {
        s->fns->destroySwapchain(s->swapchain);
    }
    for (XrSpace sp : {s->ref_space, s->view_space, s->grip_space[0], s->grip_space[1]}) {
        if (sp != XR_NULL_HANDLE) {
            s->fns->destroySpace(sp);
        }
    }
    if (s->test_pose_view != nullptr) {
        UnmapViewOfFile((const void*)s->test_pose_view);
    }
    if (s->test_pose_mapping != nullptr) {
        CloseHandle(s->test_pose_mapping);
    }
    delete s;
}

void session_note_display_period(Session* s, XrDuration period_ns) {
    if (period_ns <= 0) {
        return;
    }
    // Reject physically impossible periods outright (>1000 Hz or <5 Hz): a
    // single bogus value must never become the target the whole HUD is scaled
    // against.
    const float ms = (float)((double)period_ns / 1e6);
    if (ms < 1.0f || ms > 200.0f) {
        return;
    }
    s->period_window_ms[s->period_index] = ms;
    s->period_index = (s->period_index + 1) % Session::kPeriodWindow;
    if (s->period_count < Session::kPeriodWindow) {
        ++s->period_count;
    }

    // Runtimes mix the true panel period with longer half-rate pacing hints
    // (SteamVR motion smoothing, VDXR SSW), so the real period lives at the
    // short end of the window. Take a low percentile rather than the outright
    // minimum: the true period occupies at least half the window, while one
    // freak short reading would drag a plain min far below the panel's rate and
    // hold it there for the rest of the window.
    float sorted[Session::kPeriodWindow];
    std::copy_n(s->period_window_ms, s->period_count, sorted);
    const int nth = s->period_count / 10;
    std::nth_element(sorted, sorted + nth, sorted + s->period_count);
    const float floor_ms = sorted[nth];

    // Hysteresis keeps the HUD's colors and graph scale from flickering.
    if (s->stats.target_ms <= 0.5f ||
        std::fabs(floor_ms - s->stats.target_ms) > s->stats.target_ms * 0.05f) {
        s->stats.target_ms = floor_ms;
    }
}

void session_begin_frame(Session* s) {
    s->begin_frame_qpc_ms = qpc_ms();
}

XrResult session_end_frame(Session* s, const XrFrameEndInfo* frame_end_info) {
    const double now = qpc_ms();
    s->session_info.beat();

    // Pick up Control Center edits without restarting the game. One file
    // timestamp check per second is far cheaper than parsing every frame.
    if (now - s->last_config_check_ms > 1000.0) {
        s->last_config_check_ms = now;
        const unsigned long long stamp = Config::stamp();
        if (stamp != s->config_stamp) {
            s->config_stamp = stamp;
            if (s->config.load()) {
                flog("config reloaded");
                frame_timing::settings_changed();
            }
        }
    }

    poll_timing_source(s, now);

    update_gesture(s, frame_end_info->displayTime);

    const XrFrameEndInfo* submit_info = frame_end_info;
    XrFrameEndInfo modified{XR_TYPE_FRAME_END_INFO};
    // update_gesture only resolves an anchor when the chosen placement should
    // be visible, including persistent fallback/headset/follow modes.
    const bool placed_show = s->anchor_valid;
    const bool show = s->config.enabled && (placed_show || s->force_show);
    if (show && s->gfx != Gfx::None && frame_end_info->layerCount + 1 <= s->max_layers &&
        ensure_swapchain(s) && update_content(s)) {
        s->quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        s->quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                             XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        s->quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        s->quad.subImage.swapchain = s->swapchain;
        s->quad.subImage.imageRect = {{0, 0}, {HUD_W, HUD_H}};
        const float width_m = std::clamp(s->config.panel_width_m, 0.05f, 2.0f);
        s->quad.size = {width_m, width_m * kQuadAspect};
        if (placed_show) {
            // force_show controls visibility only. Letting it override a
            // resolved anchor would yank a controller-mounted panel to the
            // face and leave no clue as to why.
            s->quad.space = s->anchor_space;
            s->quad.pose = s->anchor_pose;
        } else {
            s->quad.space = s->view_space;
            s->quad.pose = {{0, 0, 0, 1}, {0.0f, -0.15f, -0.8f}};
        }

        s->layer_scratch.assign(frame_end_info->layers,
                                frame_end_info->layers + frame_end_info->layerCount);
        s->layer_scratch.push_back((const XrCompositionLayerBaseHeader*)&s->quad);
        modified = *frame_end_info;
        modified.layers = s->layer_scratch.data();
        modified.layerCount = (uint32_t)s->layer_scratch.size();
        submit_info = &modified;
    }

    const float cpu_submission_ms = s->begin_frame_qpc_ms > 0.0
                                        ? (float)(now - s->begin_frame_qpc_ms)
                                        : -1.0f;
    LARGE_INTEGER submit{};
    QueryPerformanceCounter(&submit);
    s->submit_qpc = uint64_t(submit.QuadPart);
    const XrResult result = s->fns->endFrame(s->session, submit_info);
    const double completed = qpc_ms();
    if (XR_SUCCEEDED(result)) {
        // This interval spans update_content above, so drop it whenever the HUD
        // repainted; SteamVR reads its frames from the compositor and is
        // unaffected either way. The clock still advances, so the next interval
        // is measured from here rather than accumulating the gap.
        if (s->stats.source != TimingBackend::SteamVR &&
            s->last_successful_end_frame_qpc_ms > 0.0 && !s->hud_refreshed_this_frame) {
            const float frame_ms = (float)(completed - s->last_successful_end_frame_qpc_ms);
            if (s->stats.source == TimingBackend::Meta) {
                s->stats.push(frame_ms, s->latest_meta_cpu_ms, s->latest_meta_gpu_ms);
            } else {
                s->stats.push(frame_ms, cpu_submission_ms, -1.0f);
            }
        }
        s->last_successful_end_frame_qpc_ms = completed;
    }
    s->hud_refreshed_this_frame = false;
    s->session_info.publish_timing(s->stats.source, s->config.timing_source, s->stats,
                                   timing_source_name(s));
    return result;
}

} // namespace xrfps
