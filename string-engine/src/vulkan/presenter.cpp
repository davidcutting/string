#include <cstdint>
#include <string/core/logger.hpp>
#include <string/vulkan/presenter.hpp>
#include <string>

#include <volk.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

namespace String
{

template<uint8_t MAX_FRAMES_IN_FLIGHT>
Presenter<MAX_FRAMES_IN_FLIGHT>::Presenter(const PresenterInfo& info)
: surface_(info.surface)
, device_(info.device)
, present_queue_(info.present_queue)
, frame_slots_()
{
    ZoneScopedN("Presenter Constructor");

    static constexpr uint32_t desired_image_count = MAX_FRAMES_IN_FLIGHT;
    uint32_t image_count = desired_image_count;

    if (info.surface_capabilities.minImageCount > desired_image_count)
    {
        STRING_LOG_WARN("Swap chain minimum image count is greater than our desired number of frames. Using the minimuim image count.");
        image_count = info.surface_capabilities.minImageCount;
    }
    else if (info.surface_capabilities.maxImageCount > 0)
    {
        image_count = std::clamp(desired_image_count,
                                 info.surface_capabilities.minImageCount,
                                 info.surface_capabilities.maxImageCount);
    }

    // clang-format off
    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = info.surface,
        .minImageCount = image_count,
        .imageFormat = info.desired_surface_format.format,
        .imageColorSpace = info.desired_surface_format.colorSpace,
        .imageExtent = info.desired_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = info.surface_capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = info.desired_present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = nullptr
    };
    // clang-format on

    // Make sure that graphics and present queues are not different queues
    QueueFamilyIndices indices = info.queue_family_indices;
    uint32_t queue_family_indices[] = {indices.graphics_family.value(), indices.present_family.value()};

    if (indices.graphics_family != indices.present_family)
    {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }

    // Save this for later...
    swapchain_create_info_ = create_info;

    if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swapchain!");
    }


    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_image_count_ = image_count;
    image_format_ = info.desired_surface_format.format;
    extent_ = info.desired_extent;

    // Create swapchain image views
    swapchain_image_views_.resize(swapchain_images_.size());
    for (uint32_t i = 0; i < swapchain_images_.size(); ++i)
    {
        swapchain_image_views_[i] = create_image_view(swapchain_images_[i], image_format_, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Create semaphores
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0
    };
    for (auto& semaphore : wait_for_image_available_semaphores_)
    {
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create semaphores during presenter creation!");
        }
    }
    for (auto& semaphore : signal_when_ready_to_present_semaphores_)
    {
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create semaphores during presenter creation!");
        }
    }
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
Presenter<MAX_FRAMES_IN_FLIGHT>::~Presenter()
{
    ZoneScopedN("Presenter Destructor");

    vkDeviceWaitIdle(device_);

    for (auto& semaphore : signal_when_ready_to_present_semaphores_)
    {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (auto& semaphore : wait_for_image_available_semaphores_)
    {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (auto& image_view : swapchain_image_views_)
    {
        if (image_view != VK_NULL_HANDLE) vkDestroyImageView(device_, image_view, nullptr);
    }
    
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
auto Presenter<MAX_FRAMES_IN_FLIGHT>::acquire_next_frame() -> AcquiredImage&
{
    ZoneScopedN("Acquire Next Frame");

    // using namespace std::chrono_literals;
    // static constexpr uint64_t acquisition_timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(1s).count();
    static constexpr uint64_t acquisition_timeout_ns = UINT64_MAX;
    static constexpr int max_acquisition_attempts = MAX_FRAMES_IN_FLIGHT;

    // Use the next frame id
    const auto& frame_id = next_frame_id_;

    // Continually retry acquisition until we hit
    for (int attempt = 0; attempt < max_acquisition_attempts; ++attempt)
    {
        const VkResult& result = vkAcquireNextImageKHR(
                device_,
                swapchain_,
                acquisition_timeout_ns,
                wait_for_image_available_semaphores_[frame_id],
                VK_NULL_HANDLE,
                &current_image_index_);

        if (result == VK_SUCCESS)
        {
            // Since we acquired, save this frame id for presentation
            current_frame_id_ = frame_id;

            // Update frame in frame_slots_
            frame_slots_[frame_id] = {
                .image = swapchain_images_.at(current_image_index_),
                .image_view = swapchain_image_views_.at(current_image_index_),
                .wait_for_image_available = wait_for_image_available_semaphores_[frame_id],
                .signal_when_ready_to_present = signal_when_ready_to_present_semaphores_[frame_id],
            };

            return frame_slots_[frame_id];
        }
        else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            resize(extent_);
            continue;
        }
        throw std::runtime_error("Failed to acquire swapchain image!");
    }
    throw std::runtime_error("Failed image acquisition after retries: " + std::to_string(max_acquisition_attempts));
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
void Presenter<MAX_FRAMES_IN_FLIGHT>::present()
{
    ZoneScopedN("Present");

    if (current_frame_id_ == std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("Usage error: present() called without a prior acquire()");
    }

    // Use the current frame
    const auto& frame_id = current_frame_id_;

    VkSwapchainKHR swap_chains[] = { swapchain_ };
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &signal_when_ready_to_present_semaphores_[frame_id],
        .swapchainCount = 1,
        .pSwapchains = swap_chains,
        .pImageIndices = &current_image_index_,
        .pResults = nullptr
    };

    const VkResult& result = vkQueuePresentKHR(present_queue_, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        resize(extent_);
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to present swapchain image!");
    }

    // FrameMark;

    // Consume this frame, we need to increment the next frame id and invalidate the current frame
    next_frame_id_= (next_frame_id_ + 1) % MAX_FRAMES_IN_FLIGHT;
    current_frame_id_ = std::numeric_limits<uint32_t>::max();
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
void Presenter<MAX_FRAMES_IN_FLIGHT>::resize(const VkExtent2D& extent)
{
    ZoneScopedN("Resize Presenter");
    vkDeviceWaitIdle(device_);

    // Destroy all the resources...
    for (auto image_view : swapchain_image_views_)
    {
        vkDestroyImageView(device_, image_view, nullptr);
    }

    // Cache the old swapchain, and update saved extents
    VkSwapchainKHR old_swapchain = swapchain_;
    extent_ = extent;
    swapchain_create_info_.imageExtent = extent;
    swapchain_create_info_.oldSwapchain = old_swapchain;

    // Recreate the swapchain...
    if (vkCreateSwapchainKHR(device_, &swapchain_create_info_, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swapchain!");
    }

    // Destroy old swapchain
    vkDestroySwapchainKHR(device_, old_swapchain, nullptr);

    // Re-intialize attachments
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count_, nullptr);
    swapchain_images_.resize(swapchain_image_count_);
    swapchain_image_views_.resize(swapchain_image_count_);
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count_, swapchain_images_.data());
    for (uint32_t i = 0; i < swapchain_images_.size(); ++i)
    {
        swapchain_image_views_[i] = create_image_view(swapchain_images_[i], image_format_, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Invalidate old image indices and frame ids
    current_frame_id_ = std::numeric_limits<uint32_t>::max();
    current_image_index_ = std::numeric_limits<uint32_t>::max();
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
auto Presenter<MAX_FRAMES_IN_FLIGHT>::get_image_count() const -> uint32_t
{
    return swapchain_image_count_;
}

template<uint8_t MAX_FRAMES_IN_FLIGHT>
auto Presenter<MAX_FRAMES_IN_FLIGHT>::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags) -> VkImageView
{
    // clang-format off
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {}, // empty component mapping?
        .subresourceRange = {
            .aspectMask = aspect_flags,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    // clang-format on

    VkImageView image_view;
    if (vkCreateImageView(device_, &view_info, nullptr, &image_view) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image view!");
    }

    return image_view;
}

}