#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

namespace String
{

struct Image {
    VkImage image;
    uint32_t width;
    uint32_t height;
    VmaAllocation allocation;
    VmaAllocationInfo allocationInfo;
};

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

    std::unique_ptr<Buffer> create_buffer(const VkDeviceSize& buffer_size, const VkBufferUsageFlags& buffer_usage, const VmaMemoryUsage& memory_usage, const VmaAllocatorCreateFlagBits& flags = {});
    std::unique_ptr<Image> create_image(const uint32_t& width, const uint32_t& height, const VkFormat& format,
        const VkImageTiling& tiling, const VkImageUsageFlags& image_usage, const VmaMemoryUsage& memory_usage);
    void destroy_buffer(std::unique_ptr<Buffer>& buffer);
    void destroy_image(std::unique_ptr<Image>& image);

    std::unique_ptr<Buffer> create_vertex_buffer(const VkDeviceSize& buffer_size);
    std::unique_ptr<Buffer> create_index_buffer(const VkDeviceSize& buffer_size);
    std::unique_ptr<Buffer> create_uniform_buffer(const VkDeviceSize& buffer_size);
    std::unique_ptr<Buffer> create_staging_buffer(const VkDeviceSize& buffer_size);

    void copy_data_to_buffer(void* data, const Buffer* buffer) const;
    
    VmaAllocator get_allocator() const;

    void print_memory_stats();
    void defragment_memory();

private:
    VkPhysicalDevice& physical_device_;
    VkDevice& device_;
    VkInstance& instance_;
    VmaAllocator allocator_;
};

}
