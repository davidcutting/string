#include <cassert>
#include <chrono>
#include <cstdint>
#include <string/vulkan/swapchain.hpp>
#include <string/core/logger.hpp>

#include <volk.h>

namespace String
{

Swapchain::Swapchain(const std::shared_ptr<Device>& device, const VkExtent2D& desired_extent)
: device_(device)
{
    SwapChainSupportDetails swap_chain_support = device_->get_swap_chain_support();

    VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swap_chain_support.formats);
    VkPresentModeKHR present_mode = choose_swap_present_mode(swap_chain_support.presentModes);
    VkExtent2D extent = choose_swap_extent(desired_extent, swap_chain_support.capabilities);

    static constexpr uint32_t desired_image_count = 2;
    uint32_t image_count = desired_image_count;

    if (swap_chain_support.capabilities.minImageCount > desired_image_count)
    {
        STRING_LOG_WARN("Swap chain minimum image count is greater than our desired number of frames. Using the minimuim image count.");
        image_count = swap_chain_support.capabilities.minImageCount;
    }
    else if (swap_chain_support.capabilities.maxImageCount > 0)
    {
        image_count = std::clamp(desired_image_count,
                                 swap_chain_support.capabilities.minImageCount,
                                 swap_chain_support.capabilities.maxImageCount);
    }

    // clang-format off
    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = device_->get_surface(),
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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

    QueueFamilyIndices indices = device_->get_queue_families();
    uint32_t queue_family_indices[] = {indices.graphics_family.value(), indices.present_family.value()};

    if (indices.graphics_family != indices.present_family)
    {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }

    if (vkCreateSwapchainKHR(device_->get_device(), &create_info, nullptr, &swap_chain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swap chain!");
    }

    vkGetSwapchainImagesKHR(device_->get_device(), swap_chain_, &image_count, nullptr);
    swap_chain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_->get_device(), swap_chain_, &image_count, swap_chain_images_.data());

    swap_chain_image_count_ = image_count;
    image_format_ = surface_format.format;
    extent_ = extent;

    swap_chain_image_views_.resize(swap_chain_images_.size());
    for (uint32_t i = 0; i < swap_chain_images_.size(); ++i)
    {
        swap_chain_image_views_[i] = create_image_view(swap_chain_images_[i], image_format_, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

Swapchain::~Swapchain()
{
    vkDeviceWaitIdle(device_->get_device());

    for (auto image_view : swap_chain_image_views_)
    {
        vkDestroyImageView(device_->get_device(), image_view, nullptr);
    }

    vkDestroySwapchainKHR(device_->get_device(), swap_chain_, nullptr);
}

std::tuple<VkResult, image_index_t> Swapchain::acquire_next_frame(const VkSemaphore& image_available_semaphore)
{
    using namespace std::chrono_literals;
    static constexpr uint64_t acquisition_timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(1s).count();

    VkResult result = vkAcquireNextImageKHR(device_->get_device(), swap_chain_, acquisition_timeout_ns,
                                            image_available_semaphore, VK_NULL_HANDLE, &current_image_);

    return {result, current_image_};
}

VkSwapchainKHR Swapchain::get_swap_chain() const
{
    return swap_chain_;
}

std::vector<VkImageView> Swapchain::get_image_views() const
{
    return swap_chain_image_views_;
}

std::vector<VkImage> Swapchain::get_images() const
{
    return swap_chain_images_;
}

uint32_t Swapchain::get_swap_chain_image_count() const
{
    return swap_chain_image_count_;
}


VkExtent2D Swapchain::get_extent() const
{
    return extent_;
}

VkImageView Swapchain::get_image_view() const
{
    return swap_chain_image_views_[current_image_];
}

VkImageView Swapchain::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags)
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
    if (vkCreateImageView(device_->get_device(), &view_info, nullptr, &image_view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image view!");
    }

    return image_view;
}

VkSurfaceFormatKHR Swapchain::choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats)
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

VkPresentModeKHR Swapchain::choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes)
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

VkExtent2D Swapchain::choose_swap_extent(const VkExtent2D& extent, const VkSurfaceCapabilitiesKHR& capabilities)
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
