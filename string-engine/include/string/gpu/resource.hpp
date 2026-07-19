#pragma once

#include <cstdint>
#include <stack>

#define VK_NO_PROTOTYPES
#include <vk_mem_alloc.h>
#include <volk.h>

namespace string::gpu
{

enum class resource_type : std::uint8_t
{
    SHADER,
    PIPELINE,
    BUFFER,
    IMAGE
};

enum class memory_type : std::uint8_t
{
    CPU_LOCAL,
    SHARED,
    GPU_LOCAL
};

enum class stream_status : std::uint8_t
{
    WANTED,
    STREAMING,
    CACHED,
    UNWANTED
};

enum class resource_priority : std::uint8_t
{
    LAZY,
    IMMEDIATE
};

using resource_id = std::uint64_t;

// Sentinel targets for the renderer-owned render targets. Passes declare their ColorWrite /
// DepthWrite against these stable logical IDs; the frame graph resolves them to the current
// backing image at record time (so recreating an attachment on resize doesn't invalidate any
// pass's declared usages). They sit at the top of the ID space, never colliding with
// allocator-assigned IDs (which count up from 0).
constexpr resource_id SWAPCHAIN_TARGET = ~resource_id{0};       // the acquired swapchain image
constexpr resource_id COLOR_TARGET     = ~resource_id{0} - 1;   // the offscreen HDR color target
constexpr resource_id DEPTH_TARGET     = ~resource_id{0} - 2;   // the offscreen depth target

struct image_info
{
    VkExtent3D extent;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect_flags;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
};

struct allocated_image
{
    resource_id id;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    VkExtent3D extent;
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
};

struct buffer_info
{
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
};

struct allocated_buffer
{
    resource_id id;
    VkBuffer buffer;
    VkDeviceSize size;
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    // GPU virtual address for shader access, non-zero only when the buffer was created with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. Lets shaders read the buffer by pointer
    // (buffer_reference) instead of a bound descriptor — the basis for GPU-driven draws.
    VkDeviceAddress device_address = 0;
};

class id_registry
{
    resource_id capacity_ = 0;
    std::stack<resource_id> free_list_;
public:
    auto get_id() -> resource_id
    {
        resource_id new_id;
        if (free_list_.empty())
        {
            new_id = capacity_;
            capacity_++;
            return new_id;
        }
        new_id = free_list_.top();
        free_list_.pop();
        return new_id;
    }
    void release_id(resource_id id)
    {
        free_list_.push(id);
    }
};

}