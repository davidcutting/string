#pragma once

#include <cstdint>
#include <stack>

#define VK_NO_PROTOTYPES
#include <vk_mem_alloc.h>
#include <volk.h>

namespace String
{

enum class ResourceType : std::uint8_t
{
    SHADER,
    PIPELINE,
    BUFFER,
    IMAGE
};

enum class MemoryType : std::uint8_t
{
    CPU_LOCAL,
    SHARED,
    GPU_LOCAL
};

enum class StreamStatus : std::uint8_t
{
    WANTED,
    STREAMING,
    CACHED,
    UNWANTED
};

enum class ResourcePriority : std::uint8_t
{
    LAZY,
    IMMEDIATE
};

using ResourceID = std::uint64_t;

// Sentinel targets for the renderer-owned render targets. Passes declare their ColorWrite /
// DepthWrite against these stable logical IDs; the frame graph resolves them to the current
// backing image at record time (so recreating an attachment on resize doesn't invalidate any
// pass's declared usages). They sit at the top of the ID space, never colliding with
// allocator-assigned IDs (which count up from 0).
constexpr ResourceID SWAPCHAIN_TARGET = ~ResourceID{0};       // the acquired swapchain image
constexpr ResourceID COLOR_TARGET     = ~ResourceID{0} - 1;   // the offscreen HDR color target
constexpr ResourceID DEPTH_TARGET     = ~ResourceID{0} - 2;   // the offscreen depth target

struct ImageInfo
{
    VkExtent3D extent;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect_flags;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
};

struct AllocatedImage
{
    ResourceID id;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    VkExtent3D extent;
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
};

struct BufferInfo
{
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
};

struct AllocatedBuffer
{
    ResourceID id;
    VkBuffer buffer;
    VkDeviceSize size;
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
};

class IDRegistry
{
    ResourceID capacity_ = 0;
    std::stack<ResourceID> free_list_;
public:
    auto get_id() -> ResourceID
    {
        ResourceID new_id;
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
    void release_id(ResourceID id)
    {
        free_list_.push(id);
    }
};

}