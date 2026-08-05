#pragma once

#include <volk.h>

namespace string::gpu
{

// Human-readable VkResult, for log lines. Covers what this engine can actually get back; anything
// else prints as its numeric code rather than pretending to know it.
const char* vk_result_name(VkResult result);

// Report a Vulkan call's result, naming the CALL SITE.
//
// Exists because a GPU fault or hang surfaces as VK_ERROR_DEVICE_LOST from whichever call happens to
// notice — submit, present, acquire, or a device-idle wait — and until now every one of those was
// either unchecked or collapsed into a bare "!= VK_SUCCESS". A device loss then looked like a
// generic failure (or nothing at all), which is useless when the symptom is a black screen on a
// machine you cannot attach a debugger to.
//
// Device loss is logged at CRITICAL, which survives a release build (INFO/WARN do not), so a shipped
// package still says what happened. Returns true if `result` was a success code.
//
// `what` should name the operation and any context worth having: "vkQueueSubmit2(graphics)",
// "vkDeviceWaitIdle(load_scene)". On a device loss that string is the only clue about which stage
// died, so make it specific.
bool vk_report(VkResult result, const char* what);

// Hand vk_report the device so a DEVICE_LOST can be interrogated with VK_EXT_device_fault, which
// reports the FAULTING GPU ADDRESS and access type (read/write/execute) instead of leaving us to
// guess which resource died. Call once after device creation; pass has_device_fault=false when the
// driver does not offer the extension and the query is skipped.
//
// Global rather than plumbed through every call site because a device loss is reported by whichever
// Vulkan call happens to notice first — submit, present, acquire, wait — and those live in four
// different classes that have no business each carrying a fault reporter.
void vk_set_fault_device(VkDevice device, bool has_device_fault);

// True for the results that mean the device is gone and every subsequent call will fail too.
[[nodiscard]] inline bool is_device_lost(VkResult result) noexcept
{
    return result == VK_ERROR_DEVICE_LOST || result == VK_ERROR_SURFACE_LOST_KHR;
}

}  // namespace string::gpu
