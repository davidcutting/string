#pragma once

#include <functional>
#include <unordered_map>

#include <string/gpu/resource.hpp>
#include <utility>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace string::gpu
{

struct ResourceAllocatorCreateInfo
{
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
};

class resource_allocator
{
    VmaAllocator allocator_;
    VkPhysicalDevice physical_device_;
    VkDevice device_;
    id_registry registry_{ 1 };   // id 0 is reserved as the "no resource" sentinel — see id_registry

    std::unordered_map<resource_id, allocated_buffer> buffers_;
    std::unordered_map<resource_id, allocated_image> images_;

public:
    explicit resource_allocator(const ResourceAllocatorCreateInfo& info);
    ~resource_allocator();

    auto create_resource(const buffer_info& info) -> resource_id;
    auto create_resource(const image_info& info) -> resource_id;
    auto create_staging(VkDeviceSize size) -> resource_id;
    void destroy_resource(resource_id id);

    auto get_buffer(resource_id id) const -> const allocated_buffer&;
    auto get_image(resource_id id) const -> const allocated_image&;

    void copy_data_to_buffer(const void* data, resource_id resource) const;

private:
    void create_image_sampler(allocated_image& allocated_image);
    void create_image_view(allocated_image& allocated_image, VkImageAspectFlags aspect_flags);
};

struct deletion_queue
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

} // namespace string::gpu