#pragma once

#include <functional>
#include <unordered_map>

#include <string/vulkan/resource.hpp>
#include <utility>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace String
{

struct ResourceAllocatorCreateInfo
{
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
};

class ResourceAllocator
{
    VmaAllocator allocator_;
    VkPhysicalDevice physical_device_;
    VkDevice device_;
    IDRegistry registry_;

    std::unordered_map<ResourceID, AllocatedBuffer> buffers_;
    std::unordered_map<ResourceID, AllocatedImage> images_;

public:
    explicit ResourceAllocator(const ResourceAllocatorCreateInfo& info);
    ~ResourceAllocator();

    auto create_resource(const BufferInfo& info) -> ResourceID;
    auto create_resource(const ImageInfo& info) -> ResourceID;
    auto create_staging(const VkDeviceSize& size) -> ResourceID;
    void destroy_resource(const ResourceID& id);

    auto get_buffer(const ResourceID& id) const -> const AllocatedBuffer&;
    auto get_image(const ResourceID& id) const -> const AllocatedImage&;

    void copy_data_to_buffer(const void* data, const ResourceID& resource) const;

private:
    void create_image_sampler(AllocatedImage& allocated_image);
    void create_image_view(AllocatedImage& allocated_image, const VkImageAspectFlags& aspect_flags);
};

struct DeletionQueue
{
    std::deque<std::function<void()>> deletors;

    void push_function(std::function<void()>&& function)
    {
        deletors.push_back(std::move(function));
    }

    void flush()
    {
        // reverse iterate the deletion queue to execute all the functions
        for (auto it = deletors.rbegin(); it != deletors.rend(); it++)
        {
            (*it)(); // call functors
        }

        deletors.clear();
    }
};

} // namespace String