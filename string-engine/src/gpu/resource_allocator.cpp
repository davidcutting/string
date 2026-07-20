#include <stdexcept>

#include <string/gpu/resource_allocator.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/resource.hpp>
#include "vulkan/vulkan_core.h"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace string::gpu
{

resource_allocator::resource_allocator(const ResourceAllocatorCreateInfo& info)
: physical_device_(info.physical_device)
, device_(info.device)
{
    VmaAllocatorCreateInfo allocator_info = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = info.physical_device,
        .device = info.device,
        .preferredLargeHeapBlockSize = 0,
        .pAllocationCallbacks = nullptr,
        .pDeviceMemoryCallbacks = nullptr,
        .pHeapSizeLimit = nullptr,
        .pVulkanFunctions = nullptr,
        .instance = info.instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
        .pTypeExternalMemoryHandleTypes = nullptr
    };

    VmaVulkanFunctions function_table;
    vmaImportVulkanFunctionsFromVolk(&allocator_info, &function_table);
    allocator_info.pVulkanFunctions = &function_table;

    if (vmaCreateAllocator(&allocator_info, &allocator_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create VMA allocator");
    }
}

resource_allocator::~resource_allocator()
{
    vkDeviceWaitIdle(device_);
    for (auto&[id, buffer] : buffers_)
    {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    }
    for (auto&[id, image] : images_)
    {
        vkDestroyImageView(device_, image.view, nullptr);
        vkDestroySampler(device_, image.sampler, nullptr);
        vmaDestroyImage(allocator_, image.image, image.allocation);
    }
    if (allocator_ != VK_NULL_HANDLE)
    {
        vmaDestroyAllocator(allocator_);
    }
}

auto resource_allocator::create_resource(const buffer_info& info) -> resource_id
{
    allocated_buffer new_buffer = {
        .id = 0,
        .buffer = VK_NULL_HANDLE,
        .size = info.size,
        .allocation = VK_NULL_HANDLE,
        .allocation_info = {},
    };

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = info.size,
        .usage = info.usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = info.memory_usage;
    alloc_info.flags = info.allocation_flags;

    VkResult result = vmaCreateBuffer(
        allocator_,
        &buffer_info,
        &alloc_info,
        &new_buffer.buffer,
        &new_buffer.allocation,
        &new_buffer.allocation_info
    );

    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create a buffer with VMA");
    }

    // Fetch the GPU virtual address for buffers that opted into device addressing, so shaders
    // can access them by pointer (buffer_reference). Only valid when the usage bit is set; the
    // allocator was created with VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT.
    if (info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    {
        const VkBufferDeviceAddressInfo address_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext = nullptr,
            .buffer = new_buffer.buffer,
        };
        new_buffer.device_address = vkGetBufferDeviceAddress(device_, &address_info);
    }

    const auto& id = registry_.get_id();
    new_buffer.id = id;
    buffers_[id] = std::move(new_buffer);
    return id;
}

auto resource_allocator::create_resource(const image_info& info) -> resource_id
{
    allocated_image new_image = {
        .id = 0,
        .image = VK_NULL_HANDLE,
        .view = VK_NULL_HANDLE,
        .sampler = VK_NULL_HANDLE,
        .format = info.format,
        .extent = info.extent,
        .allocation = VK_NULL_HANDLE,
        .allocation_info = {},
    };

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = info.format,
        .extent = {
            .width = info.extent.width,
            .height = info.extent.height,
            .depth = info.extent.depth,
        },
        .mipLevels = info.mip_levels,
        .arrayLayers = 1,
        .samples = info.samples,
        .tiling = info.tiling,
        .usage = info.usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = info.memory_usage;

    VkResult result = vmaCreateImage(
        allocator_,
        &image_info,
        &alloc_info,
        &new_image.image,
        &new_image.allocation,
        &new_image.allocation_info
    );

    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create an image with VMA");
    }

    new_image.mip_levels = info.mip_levels;
    create_image_sampler(new_image);
    create_image_view(new_image, info.aspect_flags);

    const auto& id = registry_.get_id();
    new_image.id = id;
    images_[id] = std::move(new_image);
    return id;
}

auto resource_allocator::create_staging(VkDeviceSize size) -> resource_id
{
    return create_resource(buffer_info{
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_ONLY,
        .allocation_flags = {},
    });
}

void resource_allocator::destroy_resource(resource_id id)
{
    if (buffers_.contains(id))
    {
        allocated_buffer& garbage = buffers_.at(id);
        vmaDestroyBuffer(allocator_, garbage.buffer, garbage.allocation);
        buffers_.erase(id);
    }
    if (images_.contains(id))
    {
        allocated_image& garbage = images_.at(id);
        vkDestroyImageView(device_, garbage.view, nullptr);
        vkDestroySampler(device_, garbage.sampler, nullptr);
        vmaDestroyImage(allocator_, garbage.image, garbage.allocation);
        images_.erase(id);
    }
    registry_.release_id(id);
}

auto resource_allocator::get_buffer(resource_id id) const -> const allocated_buffer&
{
    return buffers_.at(id);
}

auto resource_allocator::get_image(resource_id id) const -> const allocated_image&
{
    return images_.at(id);
}

void resource_allocator::copy_data_to_buffer(const void* data, resource_id resource) const
{
    // TODO(DCut): In general, one can configure allocations and pools with VMA to automatically contain
    // a void* to the mapping for us, which would shift the cost of mapping to the allocation time,
    // potentially saving some/tons(?) of time during this copy. Def need to profile this
    void* mapped_memory;
    auto& buffer = get_buffer(resource);
    vmaMapMemory(allocator_, buffer.allocation, &mapped_memory);
    memcpy(mapped_memory, data, buffer.size);
    vmaUnmapMemory(allocator_, buffer.allocation);
}

void resource_allocator::create_image_sampler(allocated_image& allocated_image)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.f,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.f,
        // Sample across the whole mip chain (trilinear). VK_LOD_CLAMP_NONE lets the hardware pick
        // the level from the derivative regardless of how many levels the image actually has.
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };

    if (vkCreateSampler(device_, &sampler_info, nullptr, &allocated_image.sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image sampler!");
    }
}

void resource_allocator::create_image_view(allocated_image& allocated_image, VkImageAspectFlags aspect_flags)
{
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = allocated_image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = allocated_image.format,
        .components = {},
        .subresourceRange = {
            .aspectMask = aspect_flags,
            .baseMipLevel = 0,
            .levelCount = allocated_image.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    if (vkCreateImageView(device_, &view_info, nullptr, &allocated_image.view) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image view!");
    }
}

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