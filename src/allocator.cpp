#include <vulkan/vulkan_core.h>
#include <memory>
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

    // Don't forget to throw the buffer size in there for tracking :^)
    buffer_data->buffer_size = buffer_size;

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer with VMA");
    }

    return buffer_data;
}

std::unique_ptr<Image> Allocator::create_image(const uint32_t& width, const uint32_t& height, const VkFormat& format, const VkImageTiling& tiling, const VkImageUsageFlags& image_usage, const VmaMemoryUsage& memory_usage)
{
    auto image_data = std::make_unique<Image>();

    image_data->width = width;
    image_data->height = height;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = tiling,
        .usage = image_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = memory_usage;

    VkResult result = vmaCreateImage(
        allocator_,
        &image_info,
        &alloc_info,
        &image_data->image,
        &image_data->allocation,
        &image_data->allocationInfo
    );

    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer with VMA");
    }

    return image_data;
}

void Allocator::destroy_buffer(std::unique_ptr<Buffer>& buffer)
{
    vmaDestroyBuffer(allocator_, buffer->buffer, buffer->allocation);
    buffer.reset();
}

void Allocator::destroy_image(std::unique_ptr<Image>& image)
{
    vmaDestroyImage(allocator_, image->image, image->allocation);
}

std::unique_ptr<Buffer> Allocator::create_vertex_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

std::unique_ptr<Buffer> Allocator::create_index_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

std::unique_ptr<Buffer> Allocator::create_uniform_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

std::unique_ptr<Buffer> Allocator::create_staging_buffer(const VkDeviceSize& buffer_size)
{
    return create_buffer(buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
}

void Allocator::copy_data_to_buffer(void* data, const Buffer* buffer) const
{
    void* mapped_memory;
    vmaMapMemory(allocator_, buffer->allocation, &mapped_memory);
    memcpy(mapped_memory, data, buffer->buffer_size);
    vmaUnmapMemory(allocator_, buffer->allocation);
}

VmaAllocator Allocator::get_allocator() const
{
    return allocator_;
}

// void Allocator::print_memory_stats()
// {
//     VmaStats stats;
//     vmaCalculateStats(allocator_, &stats);
    
//     std::cout << "\n=== VMA Memory Statistics ===\n";
//     std::cout << "Total allocated: " << stats.total.usedBytes / 1024 / 1024 << " MB\n";
//     std::cout << "Total unused: " << stats.total.unusedBytes / 1024 / 1024 << " MB\n";
//     std::cout << "Allocation count: " << stats.total.allocationCount << "\n";
    
//     for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i)
//     {
//         if (stats.memoryHeap[i].blockCount > 0)
//         {
//             std::cout << "Heap " << i << ": " 
//                         << stats.memoryHeap[i].usedBytes / 1024 / 1024 << " MB used, "
//                         << stats.memoryHeap[i].blockCount << " blocks\n";
//         }
//     }
// }

// void Allocator::defragment_memory() 
// {
//     VmaDefragmentationInfo2 defrag_info = {
//         .flags = VMA_DEFRAGMENTATION_FLAG_INCREMENTAL,
//         .maxCpuBytesToMove = 1024 * 1024 * 16, // 16MB max
//         .maxCpuAllocationsToMove = 32
//     };
    
//     VmaDefragmentationContext defrag_context;
    
//     if (vmaDefragmentationBegin(allocator_, &defrag_info, nullptr, &defrag_context) == VK_SUCCESS)
//     {
//         VmaDefragmentationPassInfo pass_info{};
//         result = vmaDefragmentationEnd(allocator_, defrag_context, &pass_info);
//     }
// }

}