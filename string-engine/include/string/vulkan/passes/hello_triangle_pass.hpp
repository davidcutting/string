#pragma once

#include <string/vulkan/render_pass.hpp>

#include <volk.h>

namespace String
{

struct Attachment
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

class MeshTrianglePass : public Pass
{
public:
    MeshTrianglePass(const VkDevice& device);
    ~MeshTrianglePass();

    virtual void record(const CommandRecorder& recorder) override;
};

}