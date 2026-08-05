#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hud.h"
#include "log.h"

namespace flipfps {
namespace {

constexpr float kQuadWidthM = 0.32f;
constexpr float kQuadHeightM = 0.16f;
constexpr float kQuadBottomOffsetM = 0.11f; // panel sits this far off the controller's bottom
constexpr float kFlipShowSeconds = 0.25; // hold upside down this long to show
constexpr float kUnflipHideSeconds = 0.5;
constexpr float kFlipUpY = -0.55f;       // grip +Y world-Y below this = upside down

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

bool ensure_swapchain(Session* s) {
    if (s->swapchain != XR_NULL_HANDLE) {
        return true;
    }
    if (s->gfx == Gfx::None) {
        return false;
    }

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
            return false;
        }
        void* mapped = nullptr;
        s->d12_upload->Map(0, nullptr, &mapped);
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
    if (s->gfx == Gfx::D3D12 && s->d12_fence->GetCompletedValue() < s->d12_fence_value) {
        return s->ever_uploaded; // previous copy still in flight; reuse last released image
    }

    hud_render(s->pixels.data(), s->stats, s->swapchain_bgra);

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
    return true;
}

void update_gesture(Session* s, XrTime display_time) {
    const double now = qpc_ms();

    // Optional test pose injection for the left hand (see overlay.h).
    if (!s->test_pose_env_checked) {
        s->test_pose_env_checked = true;
        char value[8]{};
        s->test_pose_env =
            GetEnvironmentVariableA("FLIPFPS_TEST_POSE", value, sizeof(value)) > 0 && value[0] == '1';
        if (s->test_pose_env) {
            flog("test pose injection enabled; waiting for shared memory");
        }
    }
    if (s->test_pose_env && s->test_pose_view == nullptr) {
        s->test_pose_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FlipFPS_TestPose");
        if (s->test_pose_mapping != nullptr) {
            s->test_pose_view =
                (const volatile float*)MapViewOfFile(s->test_pose_mapping, FILE_MAP_READ, 0, 0, 0);
            flog("test pose shared memory connected");
        }
    }

    bool flipped = false;
    XrPosef grip_pose[2];
    bool grip_valid[2]{false, false};
    for (int hand = 0; hand < 2; ++hand) {
        if (s->grip_space[hand] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(s->fns->locateSpace(s->grip_space[hand], s->ref_space, display_time, &loc))) {
            continue;
        }
        constexpr XrSpaceLocationFlags kNeeded =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
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

    for (int hand = 0; hand < 2; ++hand) {
        if (!grip_valid[hand]) {
            continue;
        }
        const V3 up = rotate(grip_pose[hand].orientation, {0.0f, 1.0f, 0.0f});
        if (!flipped && up.y < kFlipUpY) {
            flipped = true;
            s->anchor_hand = hand;
        }
    }

    // The panel is rigidly attached to the bottom of the flipped controller:
    // its orientation is the grip orientation rolled 180 degrees (so the face
    // points out of the controller's underside) and it follows the
    // controller's exact angle every frame — no billboarding.
    if (s->anchor_hand >= 0 && grip_valid[s->anchor_hand] && (flipped || s->visible)) {
        const XrPosef& grip = grip_pose[s->anchor_hand];
        s->anchor_pose.orientation = qmul(grip.orientation, XrQuaternionf{0.0f, 0.0f, 1.0f, 0.0f});
        const V3 off = rotate(grip.orientation, {0.0f, -kQuadBottomOffsetM, 0.0f});
        s->anchor_pose.position = {grip.position.x + off.x, grip.position.y + off.y,
                                   grip.position.z + off.z};
        s->anchor_valid = true;
    }

    if (flipped) {
        s->unflip_since_ms = 0.0;
        if (s->flip_since_ms == 0.0) {
            s->flip_since_ms = now;
        } else if (!s->visible && now - s->flip_since_ms > kFlipShowSeconds * 1000.0) {
            s->visible = true;
            flog("gesture: shown");
        }
    } else {
        s->flip_since_ms = 0.0;
        if (s->visible) {
            if (s->unflip_since_ms == 0.0) {
                s->unflip_since_ms = now;
            } else if (now - s->unflip_since_ms > kUnflipHideSeconds * 1000.0) {
                s->visible = false;
                flog("gesture: hidden");
            }
        }
    }

    // F10 toggles a head-locked fallback (also works when no controller binds).
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !s->f10_was_down) {
        s->force_show = !s->force_show;
        flog("F10 force-show: %d", (int)s->force_show);
    }
    s->f10_was_down = f10;
}

} // namespace

Session* session_create(XrInstance instance, XrSession session, const XrSessionCreateInfo* create_info,
                        const NextFns* fns) {
    auto* s = new Session();
    s->fns = fns;
    s->session = session;

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

    flog("session created: gfx=%s maxLayers=%u",
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
    if (period_ns > 0) {
        s->stats.target_ms = (float)((double)period_ns / 1e6);
    }
}

XrResult session_end_frame(Session* s, const XrFrameEndInfo* frame_end_info) {
    const double now = qpc_ms();
    if (s->last_end_frame_qpc_ms > 0.0) {
        s->stats.push((float)(now - s->last_end_frame_qpc_ms));
    }
    s->last_end_frame_qpc_ms = now;

    update_gesture(s, frame_end_info->displayTime);

    const bool show = (s->visible && s->anchor_valid) || s->force_show;
    if (!show || s->gfx == Gfx::None || frame_end_info->layerCount + 1 > s->max_layers) {
        return s->fns->endFrame(s->session, frame_end_info);
    }

    if (!ensure_swapchain(s) || !update_content(s)) {
        return s->fns->endFrame(s->session, frame_end_info);
    }

    s->quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    s->quad.layerFlags =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    s->quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    s->quad.subImage.swapchain = s->swapchain;
    s->quad.subImage.imageRect = {{0, 0}, {HUD_W, HUD_H}};
    s->quad.size = {kQuadWidthM, kQuadHeightM};
    if (s->visible && s->anchor_valid) {
        s->quad.space = s->ref_space;
        s->quad.pose = s->anchor_pose;
    } else {
        // Force-show fallback: head-locked, slightly below center.
        s->quad.space = s->view_space;
        s->quad.pose = {{0, 0, 0, 1}, {0.0f, -0.15f, -0.8f}};
    }

    s->layer_scratch.assign(frame_end_info->layers, frame_end_info->layers + frame_end_info->layerCount);
    s->layer_scratch.push_back((const XrCompositionLayerBaseHeader*)&s->quad);

    XrFrameEndInfo modified = *frame_end_info;
    modified.layers = s->layer_scratch.data();
    modified.layerCount = (uint32_t)s->layer_scratch.size();
    return s->fns->endFrame(s->session, &modified);
}

} // namespace flipfps
