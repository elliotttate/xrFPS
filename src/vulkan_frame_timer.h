// GPU frame timing for Vulkan sessions: the same scheme as gpu_frame_timer.h
// (a timestamp on the app's queue in xrBeginFrame and another in xrEndFrame),
// with entry points loaded from the app's own Vulkan loader.
//
// OpenXR lets a runtime - and so a layer - submit to the app's queue only
// inside xrBeginFrame, xrEndFrame and the swapchain image calls, so Begin and
// End are the only submits; Collect and Shutdown never touch the queue.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "gpu_frame_timer.h"

#include <algorithm>
#include <vector>

namespace perf::gpu {

class VulkanTimer {
public:
    VulkanTimer() = default;
    VulkanTimer(const VulkanTimer&) = delete;
    VulkanTimer& operator=(const VulkanTimer&) = delete;
    ~VulkanTimer() { Shutdown(); }

    // `calibrated`: the device was created with VK_KHR/EXT_calibrated_timestamps,
    // which puts its timestamps on the CPU clock. Without it, durations only.
    bool Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex,
              uint32_t queueIndex, bool calibrated) {
        Shutdown();
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        auto gipa = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"))
                           : nullptr;
        if (!gipa || !instance || !physicalDevice || !device) return false;
        auto instanceFn = [&](const char* name, const char* alternative = nullptr) {
            PFN_vkVoidFunction fn = gipa(instance, name);
            return fn || !alternative ? fn : gipa(instance, alternative);
        };
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceFn("vkGetDeviceProcAddr"));
        if (!gdpa) return false;
        auto deviceFn = [&](const char* name, const char* alternative = nullptr) {
            PFN_vkVoidFunction fn = gdpa(device, name);
            return fn || !alternative ? fn : gdpa(device, alternative);
        };
#define PERF_VK_INSTANCE(name, member, ...) member = reinterpret_cast<PFN_##name>(instanceFn(#name, ##__VA_ARGS__))
#define PERF_VK_DEVICE(name, member, ...) member = reinterpret_cast<PFN_##name>(deviceFn(#name, ##__VA_ARGS__))
        PERF_VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties, m_queueFamilyProperties);
        PERF_VK_INSTANCE(vkGetPhysicalDeviceProperties, m_deviceProperties);
        PERF_VK_DEVICE(vkGetDeviceQueue, m_getDeviceQueue);
        PERF_VK_DEVICE(vkCreateQueryPool, m_createQueryPool);
        PERF_VK_DEVICE(vkDestroyQueryPool, m_destroyQueryPool);
        PERF_VK_DEVICE(vkCreateCommandPool, m_createCommandPool);
        PERF_VK_DEVICE(vkDestroyCommandPool, m_destroyCommandPool);
        PERF_VK_DEVICE(vkAllocateCommandBuffers, m_allocateCommandBuffers);
        PERF_VK_DEVICE(vkBeginCommandBuffer, m_beginCommandBuffer);
        PERF_VK_DEVICE(vkEndCommandBuffer, m_endCommandBuffer);
        PERF_VK_DEVICE(vkCmdResetQueryPool, m_cmdResetQueryPool);
        PERF_VK_DEVICE(vkCmdWriteTimestamp, m_cmdWriteTimestamp);
        PERF_VK_DEVICE(vkQueueSubmit, m_queueSubmit);
        PERF_VK_DEVICE(vkGetQueryPoolResults, m_getQueryPoolResults);
        PERF_VK_DEVICE(vkCreateFence, m_createFence);
        PERF_VK_DEVICE(vkDestroyFence, m_destroyFence);
        PERF_VK_DEVICE(vkGetFenceStatus, m_getFenceStatus);
        PERF_VK_DEVICE(vkResetFences, m_resetFences);
        PERF_VK_DEVICE(vkWaitForFences, m_waitForFences);
        if (calibrated) {
            m_calibrateableDomains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                instanceFn("vkGetPhysicalDeviceCalibrateableTimeDomainsKHR",
                           "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            m_calibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                deviceFn("vkGetCalibratedTimestampsKHR", "vkGetCalibratedTimestampsEXT"));
        }
#undef PERF_VK_INSTANCE
#undef PERF_VK_DEVICE
        if (!m_queueFamilyProperties || !m_deviceProperties || !m_getDeviceQueue || !m_createQueryPool ||
            !m_destroyQueryPool || !m_createCommandPool || !m_destroyCommandPool || !m_allocateCommandBuffers ||
            !m_beginCommandBuffer || !m_endCommandBuffer || !m_cmdResetQueryPool || !m_cmdWriteTimestamp ||
            !m_queueSubmit || !m_getQueryPoolResults || !m_createFence || !m_destroyFence || !m_getFenceStatus ||
            !m_resetFences || !m_waitForFences) {
            return false;
        }

        uint32_t families = 0;
        m_queueFamilyProperties(physicalDevice, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        m_queueFamilyProperties(physicalDevice, &families, properties.data());
        const uint32_t bits = queueFamilyIndex < families ? properties[queueFamilyIndex].timestampValidBits : 0;
        VkPhysicalDeviceProperties deviceProperties{};
        m_deviceProperties(physicalDevice, &deviceProperties);
        const double periodNs = deviceProperties.limits.timestampPeriod;
        if (!bits || periodNs <= 0.0) return false;  // the app's queue family has no timestamps
        m_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        m_tickFrequency = uint64_t(1.0e9 / periodNs + 0.5);
        m_device = device;
        m_getDeviceQueue(device, queueFamilyIndex, queueIndex, &m_queue);
        if (!m_queue) {
            Shutdown();
            return false;
        }

        // One pre-recorded command buffer per query: reset it, write a timestamp.
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * 2;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queueFamilyIndex;
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = kSlots * 2;
        if (m_createQueryPool(device, &qi, nullptr, &m_pool) != VK_SUCCESS ||
            m_createCommandPool(device, &pci, nullptr, &m_commandPool) != VK_SUCCESS ||
            (cbi.commandPool = m_commandPool, m_allocateCommandBuffers(device, &cbi, m_buffers)) != VK_SUCCESS) {
            Shutdown();
            return false;
        }
        for (VkFence& fence : m_fences) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (m_createFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
                Shutdown();
                return false;
            }
        }
        for (uint32_t q = 0; q < kSlots * 2; ++q) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            m_beginCommandBuffer(m_buffers[q], &begin);
            m_cmdResetQueryPool(m_buffers[q], m_pool, q, 1);
            m_cmdWriteTimestamp(m_buffers[q], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, q);
            if (m_endCommandBuffer(m_buffers[q]) != VK_SUCCESS) {
                Shutdown();
                return false;
            }
        }
        if (m_calibrateableDomains && m_calibratedTimestamps) {
            uint32_t count = 0;
            m_calibrateableDomains(physicalDevice, &count, nullptr);
            std::vector<VkTimeDomainEXT> domains(count);
            m_calibrateableDomains(physicalDevice, &count, domains.data());
            m_qpcDomain = std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_DEVICE_EXT) != domains.end() &&
                          std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT) !=
                              domains.end();
        }
        m_slots = SlotRing{};
        Calibrate();
        return true;
    }

    bool Active() const { return m_pool != VK_NULL_HANDLE; }
    bool Calibrated() const { return m_calibration.valid; }
    uint32_t TimestampBits() const { return m_mask == ~0ull ? 64 : uint32_t(__popcnt64(m_mask)); }

    // Waits up to half a second for everything this timer put on the queue;
    // whatever is still in flight then is leaked rather than freed under the GPU.
    void Shutdown() {
        if (!m_device) return;
        bool idle = true;
        if (m_queue && m_slots.Busy()) {
            // An empty submit signals once all work ahead of it has run, begin
            // stamps included: they carry no fence of their own.
            VkFence last = VK_NULL_HANDLE;
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            idle = m_createFence(m_device, &fi, nullptr, &last) == VK_SUCCESS &&
                   m_queueSubmit(m_queue, 0, nullptr, last) == VK_SUCCESS &&
                   m_waitForFences(m_device, 1, &last, VK_TRUE, 500000000ull) == VK_SUCCESS;
            if (idle) m_destroyFence(m_device, last, nullptr);
        }
        if (idle) {
            for (VkFence& fence : m_fences) {
                if (fence) m_destroyFence(m_device, fence, nullptr);
            }
            if (m_commandPool) m_destroyCommandPool(m_device, m_commandPool, nullptr);
            if (m_pool) m_destroyQueryPool(m_device, m_pool, nullptr);
        }
        Abandon();
    }

    // Forgets every handle without touching the device: for teardown where
    // the app may already have destroyed it.
    void Abandon() {
        for (VkFence& fence : m_fences) fence = VK_NULL_HANDLE;
        m_commandPool = VK_NULL_HANDLE;
        m_pool = VK_NULL_HANDLE;
        m_queue = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_calibration = Calibration{};
        m_qpcDomain = false;
        m_slots = SlotRing{};
    }

    // xrBeginFrame, after the runtime's own: ahead of the app's frame on its queue.
    void Begin() {
        if (!Active()) return;
        const int slot = m_slots.OpenForBegin();
        if (slot >= 0) Submit(uint32_t(slot) * 2, VK_NULL_HANDLE);
    }

    // xrEndFrame entry: behind everything the app submitted for the frame.
    void End(uint64_t frameId) {
        if (!Active()) return;
        const int slot = m_slots.OpenForEnd();
        if (slot < 0) return;
        Submit(uint32_t(slot) * 2 + 1, m_fences[slot]);
        m_slots.Close(slot, frameId);
    }

    void Collect(std::vector<timeline::GpuRecord>& out) {
        if (!Active()) return;
        for (int slot = m_slots.Oldest(); slot >= 0; slot = m_slots.Oldest()) {
            // Until its submit has run, a query still holds what it held a ring ago.
            if (m_getFenceStatus(m_device, m_fences[slot]) != VK_SUCCESS) break;
            const bool begun = m_slots[slot].begun;
            struct { uint64_t value, available; } q[2] = {};
            const uint32_t first = uint32_t(slot) * 2 + (begun ? 0 : 1), count = begun ? 2 : 1;
            m_getQueryPoolResults(m_device, m_pool, first, count, sizeof(q), q, sizeof(q[0]),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (!q[count - 1].available || (begun && !q[0].available)) break;
            const uint64_t begin = begun ? q[0].value & m_mask : 0;
            uint64_t end = q[count - 1].value & m_mask;
            // A counter narrower than 64 bits wraps (36 bits at 52 ns: hourly).
            if (begun && end < begin && m_mask != ~0ull) end += m_mask + 1;
            out.push_back(MakeRecord(timeline::GpuApi::Vulkan, m_slots[slot].frameId, begin, end, m_tickFrequency,
                                     m_calibration));
            m_resetFences(m_device, 1, &m_fences[slot]);
            m_slots.Free(slot);
        }
        if (timeline::Qpc() - m_calibration.cpuQpc > QpcFrequency()) Calibrate();
    }

private:
    void Submit(uint32_t query, VkFence fence) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_buffers[query];
        m_queueSubmit(m_queue, 1, &si, fence);
    }

    void Calibrate() {
        if (!m_qpcDomain || !m_calibratedTimestamps) return;
        VkCalibratedTimestampInfoEXT infos[2] = {
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT},
        };
        uint64_t stamps[2] = {}, deviation = 0;
        if (m_calibratedTimestamps(m_device, 2, infos, stamps, &deviation) != VK_SUCCESS) return;
        m_calibration = Calibration{true, stamps[0] & m_mask, stamps[1], m_tickFrequency, QpcFrequency()};
    }

    PFN_vkGetPhysicalDeviceQueueFamilyProperties m_queueFamilyProperties{};
    PFN_vkGetPhysicalDeviceProperties m_deviceProperties{};
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT m_calibrateableDomains{};
    PFN_vkGetDeviceQueue m_getDeviceQueue{};
    PFN_vkCreateQueryPool m_createQueryPool{};
    PFN_vkDestroyQueryPool m_destroyQueryPool{};
    PFN_vkCreateCommandPool m_createCommandPool{};
    PFN_vkDestroyCommandPool m_destroyCommandPool{};
    PFN_vkAllocateCommandBuffers m_allocateCommandBuffers{};
    PFN_vkBeginCommandBuffer m_beginCommandBuffer{};
    PFN_vkEndCommandBuffer m_endCommandBuffer{};
    PFN_vkCmdResetQueryPool m_cmdResetQueryPool{};
    PFN_vkCmdWriteTimestamp m_cmdWriteTimestamp{};
    PFN_vkQueueSubmit m_queueSubmit{};
    PFN_vkGetQueryPoolResults m_getQueryPoolResults{};
    PFN_vkCreateFence m_createFence{};
    PFN_vkDestroyFence m_destroyFence{};
    PFN_vkGetFenceStatus m_getFenceStatus{};
    PFN_vkResetFences m_resetFences{};
    PFN_vkWaitForFences m_waitForFences{};
    PFN_vkGetCalibratedTimestampsEXT m_calibratedTimestamps{};

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_buffers[kSlots * 2]{};
    VkFence m_fences[kSlots]{};
    SlotRing m_slots;
    Calibration m_calibration;
    uint64_t m_tickFrequency = 0;
    uint64_t m_mask = ~0ull;
    bool m_qpcDomain = false;
};

} // namespace perf::gpu
