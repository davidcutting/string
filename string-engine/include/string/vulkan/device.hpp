#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string/platform/window.hpp>
#include <string/vulkan/driver.hpp>
#include <string/core/platform_detection.hpp>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/queue.hpp>

#include <volk.h>

namespace String
{
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class Device
{
public:
    explicit Device(Driver& driver, std::shared_ptr<Window> window);
    ~Device();

    SwapChainSupportDetails get_swap_chain_support();
    VkFormat get_format_support(const std::vector<VkFormat>& candidates, const VkImageTiling& tiling, const VkFormatFeatureFlags& features);
    VkFormat get_depth_format();
    uint32_t get_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    VkPhysicalDeviceLimits get_physical_device_limits();
    QueueFamilyIndices get_queue_families();

    auto get_surface() const -> VkSurfaceKHR;
    auto get_physical_device() -> VkPhysicalDevice&;
    auto get_device() -> VkDevice&;
    auto get_queue(QueueType type) -> Queue;

private:
    std::shared_ptr<Window> window_;
    Driver& driver_;
    VkSurfaceKHR surface_;
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};

    // Immutable per-physical-device data, queried once at selection and reused — avoids
    // re-probing queue families (2 property queries + a per-family surface-support query) on
    // every get_queue()/create_logical_device(), and re-querying properties for limits.
    QueueFamilyIndices queue_family_indices_{};
    VkPhysicalDeviceProperties properties_{};

    void select_physical_device();
    void create_logical_device();

    QueueFamilyIndices get_queue_families(const VkPhysicalDevice& physical_device);
    SwapChainSupportDetails get_swap_chain_support(const VkPhysicalDevice& physical_device);
    bool check_device_extension_support(const VkPhysicalDevice& device);
    bool is_device_suitable(const VkPhysicalDevice& physical_device);
    bool are_validation_layer_supported();

    const std::vector<const char*> validation_layers = {"VK_LAYER_KHRONOS_validation"};
    const std::vector<const char*> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
};

}