#include <stdexcept>
#include <string/allocator.hpp>
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

namespace String
{

Allocator::Allocator(VkPhysicalDevice& physical_device, VkDevice& device, VkInstance& instance)
: physical_device_(physical_device), device_(device), instance_(instance)
{
    VmaAllocatorCreateInfo allocator_info = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical_device,
        .device = device,
        .preferredLargeHeapBlockSize = 0,
        .pAllocationCallbacks = nullptr,
        .pDeviceMemoryCallbacks = nullptr,
        .pHeapSizeLimit = nullptr,
        .pVulkanFunctions = nullptr,
        .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
        .pTypeExternalMemoryHandleTypes = nullptr
    };
    
    VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }
}

Allocator::~Allocator()
{
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
    }
}

std::unique_ptr<Buffer> Allocator::create_buffer(const VkDeviceSize& buffer_size, const VkBufferUsageFlags& buffer_usage, const VmaMemoryUsage& memory_usage)
{
    auto buffer_data = std::make_unique<Buffer>();

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer_size,
        .usage = buffer_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = memory_usage;

    VkResult result = vmaCreateBuffer(
        allocator_,
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

void Allocator::destroy_buffer(std::unique_ptr<Buffer>& buffer)
{
    vmaDestroyBuffer(allocator_, buffer->buffer, buffer->allocation);
    buffer.reset();
}

std::unique_ptr<Buffer> Allocator::create_vertex_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

std::unique_ptr<Buffer> Allocator::create_uniform_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

std::unique_ptr<Buffer> Allocator::create_staging_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
}

}