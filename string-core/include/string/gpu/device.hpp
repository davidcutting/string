#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
#include <string/platform/window.hpp>
#include <string/gpu/driver.hpp>
#include <string/core/platform_detection.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/queue.hpp>

#include <volk.h>

namespace string::gpu
{
struct swap_chain_support_details {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class device
{
public:
    explicit device(driver& driver, std::shared_ptr<String::Window> window);
    ~device();

    swap_chain_support_details get_swap_chain_support();
    VkFormat get_format_support(const std::vector<VkFormat>& candidates, const VkImageTiling& tiling, const VkFormatFeatureFlags& features);
    VkFormat get_depth_format();
    uint32_t get_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    VkPhysicalDeviceLimits get_physical_device_limits();
    queue_family_indices get_queue_families();

    auto get_surface() const -> VkSurfaceKHR;
    auto get_physical_device() -> VkPhysicalDevice&;
    auto get_device() -> VkDevice&;
    auto get_queue(queue_type type) -> queue;

    // Brief 04e M1: the selected device's queue-family capability table (families, per-family
    // queue counts, capability flags, present support, timestamp validity). Enumerated once at
    // physical-device selection; the lane set and the graph's placement policy read it.
    const std::vector<queue_family_caps>& queue_capabilities() const { return family_caps_; }
    // The full useful queue set, created once at init and exposed as named submission lanes
    // ("main", "async-compute-N", "transfer"). 1:1 lane->queue; on hardware with fewer queues,
    // fewer lanes exist (no transparent multiplexing — locked guardrail).
    const std::vector<submission_lane>& submission_lanes() const { return lanes_; }
    // Lookup by name; nullptr when the hardware doesn't expose that lane.
    const submission_lane* lane(std::string_view name) const;

    // True if VK_EXT_calibrated_timestamps was available and enabled at device creation. The
    // Tracy GPU context uses this to build a calibrated (host<->device correlated) context;
    // otherwise it falls back to an uncalibrated one. Optional feature — never a hard requirement.
    bool supports_calibrated_timestamps() const { return calibrated_timestamps_enabled_; }
    bool supports_device_fault() const { return device_fault_enabled_; }

private:
    std::shared_ptr<String::Window> window_;
    driver& driver_;
    VkSurfaceKHR surface_;
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};

    // Immutable per-physical-device data, queried once at selection and reused — avoids
    // re-probing queue families (2 property queries + a per-family surface-support query) on
    // every get_queue()/create_logical_device(), and re-querying properties for limits.
    queue_family_indices queue_family_indices_{};
    // Brief 04e M1: full per-family capability table + the created submission-lane set.
    std::vector<queue_family_caps> family_caps_;
    std::vector<submission_lane> lanes_;
    // How many queues create_logical_device requested per family (indexed by family), so lane
    // construction retrieves exactly the queues that exist.
    void build_capability_table();
    void build_submission_lanes();
    VkPhysicalDeviceProperties properties_{};
    // Whether VK_EXT_calibrated_timestamps was present and enabled (optional; profiler-only).
    bool calibrated_timestamps_enabled_ = false;
    // VK_EXT_device_fault present: enables faulting-address reporting on device loss (diagnostic).
    bool device_fault_enabled_ = false;

    void select_physical_device();
    void create_logical_device();

    queue_family_indices get_queue_families(const VkPhysicalDevice& physical_device);
    swap_chain_support_details get_swap_chain_support(const VkPhysicalDevice& physical_device);
    bool check_device_extension_support(const VkPhysicalDevice& device);
    bool is_device_suitable(const VkPhysicalDevice& physical_device);
    bool are_validation_layer_supported();

    const std::vector<const char*> validation_layers = {"VK_LAYER_KHRONOS_validation"};
    const std::vector<const char*> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        // Task/mesh shader meshlet pipeline (brief 03). Locked requirement (RDNA3+), no fallback:
        // the geometry path draws through vkCmdDrawMeshTasksEXT.
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        // nullDescriptor: lets a released bindless slot be written as VK_NULL_HANDLE instead of
        // being left pointing at a destroyed image. Reads of such a descriptor return zero and
        // writes are discarded, which is defined behaviour — without it a torn-down scene leaves
        // dangling views in the set. Hard requirement, like mesh shaders: every device that can
        // run this engine supports it.
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
};

}