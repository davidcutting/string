#pragma once

#include <memory>
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

namespace String
{

struct Buffer {
    VkBuffer buffer;
    VkDeviceSize buffer_size;
    VmaAllocation allocation;
    VmaAllocationInfo allocationInfo;
};

class Allocator
{
public:
    explicit Allocator(VkPhysicalDevice& physical_device, VkDevice& device, VkInstance& instance);
    ~Allocator();

    std::unique_ptr<Buffer> create_buffer(const VkDeviceSize& buffer_size, const VkBufferUsageFlags& buffer_usage, const VmaMemoryUsage& memory_usage);
    void destroy_buffer(std::unique_ptr<Buffer>& buffer);

    std::unique_ptr<Buffer> create_vertex_buffer(const VkDeviceSize& buffer_size);
    std::unique_ptr<Buffer> create_uniform_buffer(const VkDeviceSize& buffer_size);
    std::unique_ptr<Buffer> create_staging_buffer(const VkDeviceSize& buffer_size);

private:
    VkPhysicalDevice& physical_device_;
    VkDevice& device_;
    VkInstance& instance_;
    VmaAllocator allocator_;
};

}