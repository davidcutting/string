#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <string/platform/window.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/queue.hpp>

#include <volk.h>

namespace string::gpu
{

struct acquired_image
{
    VkImage& image;
    VkImageView& image_view;
    VkSemaphore& wait_for_image_available;
    VkSemaphore& signal_when_ready_to_present;
};

class presenter
{
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    std::vector<VkSemaphore> wait_for_image_available_semaphores_;
    std::vector<VkSemaphore> signal_when_ready_to_present_semaphores_;

    device& device_;
    queue present_queue_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat image_format_;
    VkExtent2D extent_;
    // I'm only holding on to this so that resizing can use the same info as creation
    VkSwapchainCreateInfoKHR swapchain_create_info_;
    // Backing storage for swapchain_create_info_.pQueueFamilyIndices — the create info is saved
    // and reused by resize(), so the indices must outlive the constructor's stack frame.
    std::array<uint32_t, 2> shared_queue_family_indices_{};

    uint32_t current_frame_id_ = std::numeric_limits<uint32_t>::max();
    uint32_t next_frame_id_ = 0;
    uint32_t frames_in_flight_;

    uint32_t current_image_index_ = std::numeric_limits<uint32_t>::max();
    uint32_t swapchain_image_count_ = 2;

public:
    explicit presenter(device& device, const std::shared_ptr<String::Window>& window, uint32_t frames_in_flight = 3);
    ~presenter();

    [[nodiscard]]
    auto acquire_next_frame() -> acquired_image;
    void present();

    void resize(VkExtent2D extent);
    auto get_max_frames_in_flight() const -> uint32_t;
    auto get_extent() const -> VkExtent2D;
    auto get_format() const -> VkFormat { return image_format_; }

private:
    [[nodiscard]]
    auto create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags) -> VkImageView;
    auto choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats) -> VkSurfaceFormatKHR;
    auto choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes) -> VkPresentModeKHR;
    auto choose_swap_extent(VkExtent2D extent, const VkSurfaceCapabilitiesKHR& capabilities) -> VkExtent2D;
};

}