#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string/platform/window.hpp>
#include <string/vulkan/allocator.hpp>
#include <string/core/app_info.hpp>
#include <string/core/platform_detection.hpp>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/queue.hpp>

#include <volk.h>

#ifdef STRING_RELEASE
static constexpr bool ENABLE_VALIDATION_LAYERS = false;
#else
static constexpr bool ENABLE_VALIDATION_LAYERS = true;
#endif

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
    explicit Device(const ApplicationInfo& info, const std::shared_ptr<Window>& window);
    ~Device();

    SwapChainSupportDetails get_swap_chain_support();
    VkFormat get_format_support(const std::vector<VkFormat>& candidates, const VkImageTiling& tiling, const VkFormatFeatureFlags& features);
    VkFormat get_depth_format();
    uint32_t get_memory_type(const uint32_t& type_filter, const VkMemoryPropertyFlags& properties);
    VkPhysicalDeviceLimits get_physical_device_limits();
    QueueFamilyIndices get_queue_families();

    VkSurfaceKHR get_surface() const;
    VkDevice& get_device();
    auto get_queue(const QueueType& type) -> Queue;
    auto get_allocator() const -> Allocator&;

    auto create_command_recorder(const QueueType& queue_type) -> std::unique_ptr<CommandRecorder>;

private:
    std::shared_ptr<Window> window_;
    std::unique_ptr<Allocator> allocator_;
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};

    void create_instance(const ApplicationInfo& info);
    void setup_debug_messenger();
    void create_surface();
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