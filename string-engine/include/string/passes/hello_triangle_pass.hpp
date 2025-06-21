#pragma once

#include <string/device.hpp>
#include <string/render_pass.hpp>
#include <vulkan/vulkan.h>

namespace String
{

struct Attachment
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

class MeshTrianglePass : public RenderPass
{
public:
    MeshTrianglePass(const Device& device);
    ~MeshTrianglePass();

    virtual void record(const VkCommandBuffer& command_buffer, uint32_t frame_index, VkExtent2D extent) override;
    virtual void resize(VkExtent2D extent) override;
    virtual void destroy() override;

    virtual auto get_color_view() const -> VkImageView override { return color.image_view; }
    virtual auto get_color_image() const -> VkImage override { return color.image; }
    virtual auto get_color_format() const -> VkFormat override { return color_format; }

    virtual auto get_depth_view() const -> VkImageView override { return depth.image_view; }
    virtual auto get_depth_image() const -> VkImage override { return depth.image; }
    virtual auto get_depth_format() const -> VkFormat override { return depth_format; }

private:
    void create_pipeline();
    void create_attachments(VkExtent2D extent);

    Device& device_;

    VkExtent2D currentExtent{};
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    
    Attachment color;
    Attachment depth;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
};

}