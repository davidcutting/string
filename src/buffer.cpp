#include <vulkan/vulkan_core.h>
#include <string/buffer.hpp>
#include <memory>
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

namespace string
{
struct Buffer {
    VkBuffer buffer;
    VkDeviceSize buffer_size;
    VmaAllocation allocation;
    VmaAllocationInfo allocationInfo;
};

std::unique_ptr<Buffer> create_vertex_buffer(const VmaAllocator& allocator, const VkDeviceSize& buffer_size)
{
    auto buffer_data = std::make_unique<Buffer>();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    VkResult result = vmaCreateBuffer(
        allocator,
        &buffer_info,
        &alloc_info,
        &buffer_data->buffer,
        &buffer_data->allocation,
        &buffer_data->allocationInfo
    );

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer with VMA");
    }

    return buffer_data;
}

std::unique_ptr<Buffer> create_uniform_buffer(const VmaAllocator& allocator, const VkDeviceSize& buffer_size)
{
    auto buffer_data = std::make_unique<Buffer>();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT; // Keep persistently mapped
    
    VkResult result = vmaCreateBuffer(
        allocator,
        &buffer_info,
        &alloc_info,
        &buffer_data->buffer,
        &buffer_data->allocation,
        &buffer_data->allocationInfo
    );

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create uniform buffer");
    }

    return buffer_data;
}

std::unique_ptr<Buffer> create_staging_buffer(const VmaAllocator& allocator, const VkDeviceSize& buffer_size)
{
    auto buffer_data = std::make_unique<Buffer>();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    
    VkResult result = vmaCreateBuffer(
        allocator,
        &buffer_info,
        &alloc_info,
        &buffer_data->buffer,
        &buffer_data->allocation,
        &buffer_data->allocationInfo
    );

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer");
    }

    return buffer_data;
}

std::unique_ptr<Buffer> create_buffer(const VmaAllocator& allocator, const VkDeviceSize& buffer_size, const VkBufferUsageFlags& usage)
{
    auto buffer_data = std::make_unique<Buffer>();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer_size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    VkResult result = vmaCreateBuffer(
        allocator,
        &buffer_info,
        &alloc_info,
        &buffer_data->buffer,
        &buffer_data->allocation,
        &buffer_data->allocationInfo
    );

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer with VMA");
    }

    return buffer_data;
}

void dma_vertex_buffer_to_gpu(const VmaAllocator& allocator, const std::unique_ptr<Buffer>& buffer)
{
    void* mapped_buffer_data;
    vmaMapMemory(allocator, buffer->allocation, &mapped_buffer_data);
    memcpy(mapped_buffer_data, vertices.data(), buffer->allocation->GetSize());
    vmaUnmapMemory(allocator, buffer->allocation);
}



void destroy_buffer(const VmaAllocator& allocator, std::unique_ptr<Buffer>& buffer)
{
    vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
    buffer.reset();
}

}