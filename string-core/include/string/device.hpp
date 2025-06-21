#pragma once

#include <string/platform/window.hpp>

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace String
{

enum class DeviceType : std::uint8_t
{
    DISCRETE,
    INTEGRATED,
    CPU,
    VIRTUAL,
    ANY
};

struct SwapchainInfo {
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkPresentModeKHR present_mode;
    bool vsync_enabled;
};

struct PresentInfo {
    uint32_t image_index;
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<VkSemaphore> signal_semaphores;
    VkFence fence = VK_NULL_HANDLE;
};

struct AcquireInfo {
    uint32_t image_index;
    VkSemaphore semaphore;
    bool swapchain_recreated;
};

struct DeviceConfig
{

};

class Device
{
public:
    Device(const DeviceConfig& desc, VkInstance instance, const Window* window = nullptr);
    ~Device();
    
    // Disable copy and move
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;
    
    // Device info
    bool is_headless() const;
    std::string get_device_name() const;
    DeviceType get_device_type() const;
    uint64_t get_memory_size() const;
    
    // Vulkan objects
    VkDevice get_device() const;
    VkPhysicalDevice get_physical_device() const;
    uint32_t get_graphics_queue_family() const;
    uint32_t get_present_queue_family() const;
    VkQueue get_graphics_queue() const;
    VkQueue get_present_queue() const;
    
    // Presentation (only available for non-headless devices)
    // AcquireInfo acquire_next_image(uint64_t timeout = UINT64_MAX);
    // void present(const PresentInfo& info);
    // SwapchainInfo get_swapchain_info() const;
    std::vector<VkImage> get_swapchain_images() const;
    std::vector<VkImageView> get_swapchain_image_views() const;
    
    // Synchronization helpers
    VkSemaphore get_current_acquire_semaphore() const;
    VkSemaphore get_current_present_semaphore() const;
    uint64_t get_current_timeline_value() const;
    
    // Resource management
    void wait_idle();
    void wait_for_timeline(uint64_t value, uint64_t timeout = UINT64_MAX);
};

}