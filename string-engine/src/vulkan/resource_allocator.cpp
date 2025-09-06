#include <stdexcept>

#include <string/vulkan/resource_allocator.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/resource.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace String
{

ResourceAllocator::ResourceAllocator(const ResourceAllocatorCreateInfo& info)
: device_(info.device)
, allocator_(info.allocator)
{
    // VmaAllocatorCreateInfo allocator_info = {
    //     .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
    //     .physicalDevice = info.physical_device,
    //     .device = info.device,
    //     .preferredLargeHeapBlockSize = 0,
    //     .pAllocationCallbacks = nullptr,
    //     .pDeviceMemoryCallbacks = nullptr,
    //     .pHeapSizeLimit = nullptr,
    //     .pVulkanFunctions = nullptr,
    //     .instance = info.instance,
    //     .vulkanApiVersion = VK_API_VERSION_1_3,
    //     .pTypeExternalMemoryHandleTypes = nullptr
    // };

    // VmaVulkanFunctions function_table;
    // vmaImportVulkanFunctionsFromVolk(&allocator_info, &function_table);
    // allocator_info.pVulkanFunctions = &function_table;

    // if (vmaCreateAllocator(&allocator_info, &allocator_) != VK_SUCCESS)
    // {
    //     throw std::runtime_error("Failed to create VMA allocator");
    // }

    VkSemaphoreTypeCreateInfo timeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = timeline_value_,
    };

    VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_create_info,
        .flags = 0,
    };

    if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create timeline semaphore for resource allocator!");
    }
}

ResourceAllocator::~ResourceAllocator()
{
    vkDeviceWaitIdle(device_);
    for (auto&[id, buffer] : buffers_)
    {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    }
    for (auto&[id, image] : images_)
    {
        vmaDestroyImage(allocator_, image.image, image.allocation);
    }
    if (timeline_semaphore_ != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
    }
    // if (allocator_ != VK_NULL_HANDLE)
    // {
    //     vmaDestroyAllocator(allocator_);
    // }
}

auto ResourceAllocator::create_buffer(const BufferInfo& info) -> ResourceID
{
    AllocatedBuffer new_buffer = {
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

    const auto& id = registry_.get_id();
    buffers_[id] = std::move(new_buffer);
    return id;
}

void ResourceAllocator::destroy_buffer(const ResourceID& id)
{
    PendingRelease release = {
        .id = id,
        .release_value = timeline_value_,
    };

    pending_buffers_.emplace(std::move(release));
}

auto ResourceAllocator::get_buffer(const ResourceID& id) const -> const AllocatedBuffer&
{
    return buffers_.at(id);
}

auto ResourceAllocator::create_image(const ImageInfo& info) -> ResourceID
{
    AllocatedImage new_image = {
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
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
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

    const auto& id = registry_.get_id();
    images_[id] = std::move(new_image);
    return id;
}

void ResourceAllocator::destroy_image(const ResourceID& id)
{
    PendingRelease release = {
        .id = id,
        .release_value = timeline_value_,
    };

    pending_images_.emplace(std::move(release));
}

auto ResourceAllocator::get_image(const ResourceID& id) const -> const AllocatedImage&
{
    return images_.at(id);
}

auto ResourceAllocator::create_transient_buffer(const BufferInfo& info) -> ResourceID
{
    // TODO: implement
}

void ResourceAllocator::destroy_transient_buffer(const ResourceID& id)
{
    // TODO: implement
}

template<typename T>
auto get_transient_buffer(const ResourceID& id) -> std::span<T>
{
    // TODO: implement
}

void ResourceAllocator::flush()
{
    uint64_t completed_value = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &completed_value) != VK_SUCCESS)
    {
        STRING_LOG_ERROR("Failed to get timeline semaphore value during ResourceAllocator::flush(). Skipping flush...");
        return;
    }

    while (!pending_buffers_.empty())
    {
        const auto& release = pending_buffers_.front();
        if (release.release_value > completed_value)
        {
            break;
        }

        AllocatedBuffer& garbage = buffers_.at(release.id);
        vmaDestroyBuffer(allocator_, garbage.buffer, garbage.allocation);

        buffers_.erase(release.id);
        registry_.release_id(release.id);

        pending_buffers_.pop();
    }

    while (!pending_images_.empty())
    {
        const auto& release = pending_images_.front();
        if (release.release_value > completed_value)
        {
            break;
        }

        AllocatedImage& garbage = images_.at(release.id);
        vmaDestroyImage(allocator_, garbage.image, garbage.allocation);

        images_.erase(release.id);
        registry_.release_id(release.id);

        pending_images_.pop();
    }
}

auto ResourceAllocator::advance_timeline() -> VkTimelineSemaphoreSubmitInfo
{
    timeline_value_++;

    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 0,
        .pWaitSemaphoreValues = nullptr,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &timeline_value_,
    };

    return timeline_info;
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