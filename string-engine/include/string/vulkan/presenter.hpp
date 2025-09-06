#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <string/vulkan/device.hpp>
#include <string/vulkan/queue.hpp>
#include "vulkan/vulkan_core.h"

#include <volk.h>

namespace String
{

struct AcquiredImage
{
    VkImage& image;
    VkImageView& image_view;
    VkSemaphore& wait_for_image_available;
    VkSemaphore& signal_when_ready_to_present;
};

struct PresenterInfo
{
    VkSurfaceKHR& surface;
    VkDevice& device;
    VkQueue& present_queue;
    VkExtent2D desired_extent;
    VkSurfaceFormatKHR desired_surface_format;
    VkSurfaceCapabilitiesKHR surface_capabilities;
    VkPresentModeKHR desired_present_mode;
    QueueFamilyIndices queue_family_indices;
    uint32_t desired_image_count;
};

template<uint8_t MAX_FRAMES_IN_FLIGHT = 3>
class Presenter
{
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    std::array<AcquiredImage, MAX_FRAMES_IN_FLIGHT> frame_slots_;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> wait_for_image_available_semaphores_;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> signal_when_ready_to_present_semaphores_;

    VkSurfaceKHR& surface_;
    VkDevice& device_;
    VkQueue& present_queue_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat image_format_;
    VkExtent2D extent_;
    // I'm only holding on to this so that resizing can use the same info as creation
    VkSwapchainCreateInfoKHR swapchain_create_info_;

    uint32_t current_frame_id_ = std::numeric_limits<uint32_t>::max();
    uint32_t next_frame_id_ = 0;
    uint32_t current_image_index_ = std::numeric_limits<uint32_t>::max();
    uint32_t swapchain_image_count_ = MAX_FRAMES_IN_FLIGHT;

public:
    explicit Presenter(const PresenterInfo& info);
    ~Presenter();

    [[nodiscard]]
    auto acquire_next_frame() -> AcquiredImage&;
    void present();

    void resize(const VkExtent2D& extent);
    [[nodiscard]]
    auto get_image_count() const -> uint32_t;

private:
    [[nodiscard]]
    auto create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags) -> VkImageView;
};

}