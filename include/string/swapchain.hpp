#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <memory>
#include <string/device.hpp>
#include <vector>

namespace String
{

using image_index_t = std::uint32_t;

class Swapchain
{
public:
    explicit Swapchain(const std::shared_ptr<Device>& device, const VkExtent2D& desired_extent);
    ~Swapchain();

    std::tuple<VkResult, image_index_t> acquire_next_frame(const VkSemaphore& image_available_semaphore);

    VkSwapchainKHR get_swap_chain() const;
    std::vector<VkImageView> get_image_views() const;
    std::vector<VkImage> get_images() const;
    uint32_t get_swap_chain_image_count() const;
    VkExtent2D get_extent() const;
    VkImageView get_image_view() const;

private:
    std::shared_ptr<Device> device_;

    VkSwapchainKHR swap_chain_{VK_NULL_HANDLE};
    std::vector<VkImage> swap_chain_images_;
    std::vector<VkImageView> swap_chain_image_views_;
    VkFormat image_format_;
    VkExtent2D extent_;
    image_index_t current_image_{0};
    uint32_t swap_chain_image_count_{2};

    VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags);
    VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats);
    VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes);
    VkExtent2D choose_swap_extent(const VkExtent2D& extent, const VkSurfaceCapabilitiesKHR& capabilities);
};

}