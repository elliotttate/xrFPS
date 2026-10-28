#pragma once
// The frame timeline: every frame's timing, published to shared memory
// (Local\xrFPS.FrameTimeline.<pid>, layout in frame_timeline.h) for the
// frame-time viewer. On a real runtime the layer sees the frame calls from the
// outside, so it stamps around the calls it passes down:
//
//   xrWaitFrame   the whole call is the runtime's pacing wait
//   xrBeginFrame  time inside the runtime's call is blocked, not the app's
//   xrEndFrame    xrFPS's own HUD work, then the runtime's submission
//
// and puts a GPU timestamp on the app's queue in xrBeginFrame and in
// xrEndFrame, ahead of the HUD upload, for the frame's GPU time.
//
// XRFPS_FRAME_TIMELINE=0 turns the timeline off. GPU timing follows the
// Control Center's setting unless XRFPS_GPU_TIMING=0/1 pins it.
#include "frame_timeline.h"

#include <openxr/openxr.h>

namespace xrfps {

struct Session;

namespace frame_timing {

// Instance: after the downchain instance exists.
void instance_created(XrInstance instance, PFN_xrGetInstanceProcAddr next_gipa, const char* app_name);
void instance_destroyed();

// Session: after the runtime created it / before it is destroyed.
// `device_may_be_gone`: torn down with the instance, not by xrDestroySession.
void session_created(Session* s, XrInstance instance, const XrSessionCreateInfo* create_info);
void session_destroyed(Session* s, bool device_may_be_gone = false);

void session_state_changed(XrSessionState state);
void swapchain_created(const XrSwapchainCreateInfo& info);
void settings_changed();

// Frame calls, around the downchain call.
void wait_enter(perf::timeline::WaitSample& wait);
void wait_exit(Session* s, perf::timeline::WaitSample& wait, const XrFrameState& state);
void wait_failed();
void begin_enter();
void begin_exit(Session* s, bool succeeded);
void locate(XrTime display_time);
void end_enter(Session* s, perf::timeline::FrameRecord& frame, const XrFrameEndInfo& info);
// Reads s->submit_qpc and s->end_return_qpc, which session_end_frame stamps.
void end_exit(Session* s, perf::timeline::FrameRecord& frame, const XrFrameEndInfo& info, XrResult result);

// xrCreateVulkanDeviceKHR (XR_KHR_vulkan_enable2): asks for calibrated
// timestamps so Vulkan GPU times land on the CPU clock.
PFN_xrVoidFunction vulkan_device_hook();

} // namespace frame_timing
} // namespace xrfps
