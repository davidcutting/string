#include <cstdint>
#include <memory>
#include <string>
#include <cassert>

#include <string/core/profiler.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/presenter.hpp>
#include <string/vulkan/queue.hpp>

#include <volk.h>

namespace String
{

Presenter::Presenter(Device& device, std::shared_ptr<Window> window, uint32_t frames_in_flight)
: device_(device)
, present_queue_(device_.get_queue(QueueType::PRESENT))
, wait_for_image_available_semaphores_{VK_NULL_HANDLE}
, signal_when_ready_to_present_semaphores_{VK_NULL_HANDLE}
, frames_in_flight_(frames_in_flight)
{
    STRING_PROFILE_SCOPE("Presenter Constructor")

    SwapChainSupportDetails swap_chain_support = device_.get_swap_chain_support();
    VkExtent2D desired_extent = {
        .width = window->get_extent().width,
        .height = window->get_extent().height,
    };

    VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swap_chain_support.formats);
    VkPresentModeKHR present_mode = choose_swap_present_mode(swap_chain_support.presentModes);
    VkExtent2D extent = choose_swap_extent(desired_extent, swap_chain_support.capabilities);

    uint32_t image_count = frames_in_flight;

    if (swap_chain_support.capabilities.minImageCount > image_count)
    {
        STRING_LOG_WARN("Swap chain minimum image count is greater than our desired number of frames. Using the minimuim image count.");
        image_count = swap_chain_support.capabilities.minImageCount;
    }
    else if (swap_chain_support.capabilities.maxImageCount > 0)
    {
        image_count = std::clamp(image_count,
                                 swap_chain_support.capabilities.minImageCount,
                                 swap_chain_support.capabilities.maxImageCount);
    }

    // clang-format off
    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = device_.get_surface(),
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        // TRANSFER_DST so the renderer can blit its offscreen target into the swapchain image.
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = swap_chain_support.capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = nullptr
    };
    // clang-format on

    // Make sure that graphics and present queues are not different queues
    QueueFamilyIndices indices = device_.get_queue_families();
    uint32_t queue_family_indices[] = {indices.graphics_family.value(), indices.present_family.value()};

    if (indices.graphics_family != indices.present_family)
    {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }

    // Save this for later...
    swapchain_create_info_ = create_info;

    if (vkCreateSwapchainKHR(device_.get_device(), &create_info, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swapchain!");
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device_.get_device(), swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_.get_device(), swapchain_, &image_count, swapchain_images_.data());

    swapchain_image_count_ = image_count;
    image_format_ = surface_format.format;
    extent_ = extent;

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
    wait_for_image_available_semaphores_.resize(frames_in_flight);
    for (auto& semaphore : wait_for_image_available_semaphores_)
    {
        if (vkCreateSemaphore(device_.get_device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create semaphores during presenter creation!");
        }
    }
    signal_when_ready_to_present_semaphores_.resize(image_count);
    for (auto& semaphore : signal_when_ready_to_present_semaphores_)
    {
        if (vkCreateSemaphore(device_.get_device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create semaphores during presenter creation!");
        }
    }
}

Presenter::~Presenter()
{
    STRING_PROFILE_SCOPE("Presenter Destructor")

    for (auto& semaphore : signal_when_ready_to_present_semaphores_)
    {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_.get_device(), semaphore, nullptr);
    }
    for (auto& semaphore : wait_for_image_available_semaphores_)
    {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_.get_device(), semaphore, nullptr);
    }
    for (auto& image_view : swapchain_image_views_)
    {
        if (image_view != VK_NULL_HANDLE) vkDestroyImageView(device_.get_device(), image_view, nullptr);
    }
    
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_.get_device(), swapchain_, nullptr);
}

auto Presenter::acquire_next_frame() -> AcquiredImage
{
    STRING_PROFILE_SCOPE("Acquire Next Frame")

    // using namespace std::chrono_literals;
    // static constexpr uint64_t acquisition_timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(1s).count();
    static constexpr uint64_t acquisition_timeout_ns = UINT64_MAX;
    static int max_acquisition_attempts = frames_in_flight_;

    // Use the next frame id
    const auto& frame_id = next_frame_id_;

    // Continually retry acquisition until we hit
    for (int attempt = 0; attempt < max_acquisition_attempts; ++attempt)
    {
        const VkResult& result = vkAcquireNextImageKHR(
                device_.get_device(),
                swapchain_,
                acquisition_timeout_ns,
                wait_for_image_available_semaphores_[frame_id],
                VK_NULL_HANDLE,
                &current_image_index_);

        if (result == VK_SUCCESS)
        {
            current_frame_id_ = frame_id;

            return AcquiredImage {
                .image = swapchain_images_.at(current_image_index_),
                .image_view = swapchain_image_views_.at(current_image_index_),
                .wait_for_image_available = wait_for_image_available_semaphores_[frame_id],
                .signal_when_ready_to_present = signal_when_ready_to_present_semaphores_[current_image_index_],
            };
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

void Presenter::present()
{
    STRING_PROFILE_SCOPE("Present")

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
        .pWaitSemaphores = &signal_when_ready_to_present_semaphores_[current_image_index_],
        .swapchainCount = 1,
        .pSwapchains = swap_chains,
        .pImageIndices = &current_image_index_,
        .pResults = nullptr
    };

    const VkResult& result = vkQueuePresentKHR(present_queue_.queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        resize(extent_);
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to present swapchain image!");
    }

    STRING_MARK_FRAME

    // Consume this frame, we need to increment the next frame id and invalidate the current frame
    next_frame_id_= (next_frame_id_ + 1) % frames_in_flight_;
    current_frame_id_ = std::numeric_limits<uint32_t>::max();
}

void Presenter::resize(const VkExtent2D& extent)
{
    STRING_PROFILE_SCOPE("Resize Presenter")
    vkDeviceWaitIdle(device_.get_device());

    // Destroy all the resources...
    for (auto image_view : swapchain_image_views_)
    {
        vkDestroyImageView(device_.get_device(), image_view, nullptr);
    }

    // Cache the old swapchain, and update saved extents
    VkSwapchainKHR old_swapchain = swapchain_;
    extent_ = extent;
    swapchain_create_info_.imageExtent = extent;
    swapchain_create_info_.oldSwapchain = old_swapchain;

    // Recreate the swapchain...
    if (vkCreateSwapchainKHR(device_.get_device(), &swapchain_create_info_, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swapchain!");
    }

    // Destroy old swapchain
    vkDestroySwapchainKHR(device_.get_device(), old_swapchain, nullptr);

    // Re-intialize attachments
    vkGetSwapchainImagesKHR(device_.get_device(), swapchain_, &swapchain_image_count_, nullptr);
    swapchain_images_.resize(swapchain_image_count_);
    swapchain_image_views_.resize(swapchain_image_count_);
    vkGetSwapchainImagesKHR(device_.get_device(), swapchain_, &swapchain_image_count_, swapchain_images_.data());
    for (uint32_t i = 0; i < swapchain_images_.size(); ++i)
    {
        swapchain_image_views_[i] = create_image_view(swapchain_images_[i], image_format_, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Re-initialize synchronization
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0
    };
    signal_when_ready_to_present_semaphores_.resize(swapchain_image_count_);
    for (auto& semaphore : signal_when_ready_to_present_semaphores_)
    {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_.get_device(), semaphore, nullptr);
        if (vkCreateSemaphore(device_.get_device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create presenter semaphores during resize!");
        }
    }

    // Invalidate old image indices and frame ids
    current_frame_id_ = std::numeric_limits<uint32_t>::max();
    current_image_index_ = std::numeric_limits<uint32_t>::max();
}

auto Presenter::get_max_frames_in_flight() const -> uint32_t
{
    return frames_in_flight_;
}

auto Presenter::get_extent() const -> VkExtent2D
{
    return extent_;
}

auto Presenter::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags) -> VkImageView
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
    if (vkCreateImageView(device_.get_device(), &view_info, nullptr, &image_view) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image view!");
    }

    return image_view;
}

auto Presenter::choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats) -> VkSurfaceFormatKHR
{
    for (const auto& available_format : available_formats)
    {
        if (available_format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            available_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return available_format;
        }
    }

    return available_formats[0];
}

auto Presenter::choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes) -> VkPresentModeKHR
{
    assert(available_present_modes.size() > 0 && "Must have available present modes.");

    for (const auto& present_mode : available_present_modes)
    {
        // Note: This switch is here to show a list of priority in present modes. I did it this way
        //       so that you may change it if you like; however, the FIFO present mode is always
        //       supported by devices, so it will always be caught in this switch case if other modes
        //       are not listed before it.
        switch (present_mode)
        {
            case VK_PRESENT_MODE_MAILBOX_KHR:
                STRING_LOG_INFO("Present Mode: Mailbox");
                return present_mode;  // Mailbox
            // Note: There are other present modes as of this comment's authoring, but they will
            //       not be supported as they require much more syncronization by the swapchain,
            //       and these cases are way more convenient.
            default:
                STRING_LOG_INFO("Present Mode: FIFO");
                return VK_PRESENT_MODE_FIFO_KHR;  // FIFO mode is required to be supported
        }
    }
    // TODO(DCut): Remove this return and silence warnings instead
    STRING_LOG_ERROR("Strange things happened here, but present mode is FIFO");
    return VK_PRESENT_MODE_FIFO_KHR;  // FIFO mode is required to be supported
}

auto Presenter::choose_swap_extent(const VkExtent2D& extent, const VkSurfaceCapabilitiesKHR& capabilities) -> VkExtent2D
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
        return capabilities.currentExtent;
    }

    VkExtent2D actual_extent{};

    actual_extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actual_extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return actual_extent;
}

}