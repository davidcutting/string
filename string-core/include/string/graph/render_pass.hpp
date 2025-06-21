#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace String
{

class RenderPass
{
public:
    virtual ~RenderPass() = default;

    virtual void record(const VkCommandBuffer& command_buffer, uint32_t frame_index, VkExtent2D extent) = 0;
    virtual void resize(VkExtent2D extent) = 0;
    virtual void destroy() = 0;

    virtual auto get_color_view() const -> VkImageView = 0;
    virtual auto get_color_image() const -> VkImage = 0;
    virtual auto get_color_format() const -> VkFormat = 0;

    virtual auto get_depth_view() const -> VkImageView = 0;
    virtual auto get_depth_image() const -> VkImage = 0;
    virtual auto get_depth_format() const -> VkFormat = 0;
};

}