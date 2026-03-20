#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <string/platform/window.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/queue.hpp>

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

class Presenter
{
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    std::vector<VkSemaphore> wait_for_image_available_semaphores_;
    std::vector<VkSemaphore> signal_when_ready_to_present_semaphores_;

    Device& device_;
    Queue present_queue_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat image_format_;
    VkExtent2D extent_;
    // I'm only holding on to this so that resizing can use the same info as creation
    VkSwapchainCreateInfoKHR swapchain_create_info_;

    uint32_t current_frame_id_ = std::numeric_limits<uint32_t>::max();
    uint32_t next_frame_id_ = 0;
    uint32_t frames_in_flight_;

    uint32_t current_image_index_ = std::numeric_limits<uint32_t>::max();
    uint32_t swapchain_image_count_ = 2;

public:
    explicit Presenter(Device& device, std::shared_ptr<Window> window, uint32_t frames_in_flight = 3);
    ~Presenter();

    [[nodiscard]]
    auto acquire_next_frame() -> AcquiredImage;
    void present();

    void resize(const VkExtent2D& extent);
    auto get_max_frames_in_flight() const -> uint32_t;
    auto get_extent() const -> VkExtent2D;

private:
    [[nodiscard]]
    auto create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags) -> VkImageView;
    auto choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats) -> VkSurfaceFormatKHR;
    auto choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes) -> VkPresentModeKHR;
    auto choose_swap_extent(const VkExtent2D& extent, const VkSurfaceCapabilitiesKHR& capabilities) -> VkExtent2D;
};

}