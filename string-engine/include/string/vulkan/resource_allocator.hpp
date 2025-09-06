#pragma once

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <span>

#include <string/vulkan/resource.hpp>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace String
{

struct PendingRelease
{
    ResourceID id;
    uint64_t release_value;
};

struct ResourceAllocatorCreateInfo
{
    VkDevice& device;
    VmaAllocator& allocator;
};

class ResourceAllocator
{
    VmaAllocator& allocator_;
    VkDevice& device_;
    IDRegistry registry_;

    std::unordered_map<ResourceID, AllocatedBuffer> buffers_;
    std::unordered_map<ResourceID, AllocatedImage> images_;

    std::queue<PendingRelease> pending_buffers_;
    std::queue<PendingRelease> pending_images_;

    VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
    uint64_t timeline_value_ = 1;

public:
    explicit ResourceAllocator(const ResourceAllocatorCreateInfo& info);
    ~ResourceAllocator();

    auto create_buffer(const BufferInfo& info) -> ResourceID;
    void destroy_buffer(const ResourceID& id);
    auto get_buffer(const ResourceID& id) const -> const AllocatedBuffer&;

    auto create_image(const ImageInfo& info) -> ResourceID;
    void destroy_image(const ResourceID& id);
    auto get_image(const ResourceID& id) const -> const AllocatedImage&;

    auto create_transient_buffer(const BufferInfo& info) -> ResourceID;
    void destroy_transient_buffer(const ResourceID& id);
    template<typename T>
    auto get_transient_buffer(const ResourceID& id) -> std::span<T>;

    void flush();
    auto advance_timeline() -> VkTimelineSemaphoreSubmitInfo;
};

} // namespace String