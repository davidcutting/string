#pragma once

#include <cstdint>
#include <memory>
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
    VkPhysicalDeviceProperties properties_{};

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
        VK_EXT_MESH_SHADER_EXTENSION_NAME};
};

}