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
        // A sub-resource view SHARES its source's VkImage and allocation — it owns only its own view.
        // Freeing them here as well is a double free, and it faults inside VMA rather than where the
        // mistake is. destroy_resource() has always honoured this; the destructor did not.
        // (Samplers are not per-image any more — they are shared out of samplers_ and destroyed below.)
        if (!image.is_view)
        {
            vmaDestroyImage(allocator_, image.image, image.allocation);
        }
    }
    for (const auto& [info, sampler] : samplers_)
    {
        vkDestroySampler(device_, sampler, nullptr);
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
        .flags = info.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : VkImageCreateFlags{ 0 },
        .imageType = VK_IMAGE_TYPE_2D,
        .format = info.format,
        .extent = {
            .width = info.extent.width,
            .height = info.extent.height,
            .depth = info.extent.depth,
        },
        .mipLevels = info.mip_levels,
        .arrayLayers = info.cube ? 6u : 1u,
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
    new_image.array_layers = info.cube ? 6u : 1u;
    new_image.samples = info.samples;
    create_image_sampler(new_image, info.sampler);
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
    // 0 is the "no resource" sentinel, never a real id (see id_registry). Pass dtors destroy their
    // optional members unconditionally, so this is the common case, not an error.
    if (id == 0)
    {
        return;
    }

    bool destroyed = false;
    if (buffers_.contains(id))
    {
        allocated_buffer& garbage = buffers_.at(id);
        vmaDestroyBuffer(allocator_, garbage.buffer, garbage.allocation);
        buffers_.erase(id);
        destroyed = true;
    }
    if (images_.contains(id))
    {
        allocated_image& garbage = images_.at(id);
        vkDestroyImageView(device_, garbage.view, nullptr);
        // A sub-resource view owns nothing but its VkImageView — the image and allocation belong to
        // the source image it slices. The sampler belongs to neither: it is shared out of samplers_.
        if (!garbage.is_view)
        {
            vmaDestroyImage(allocator_, garbage.image, garbage.allocation);
        }
        images_.erase(id);
        destroyed = true;
    }

    // Recycle the id ONLY if this call actually owned something. Releasing unconditionally made a
    // double-destroy push the same id onto the free list twice, so two live resources could later be
    // handed the SAME id — the second `buffers_[id] = ...` then overwrote the first's entry, leaking
    // its VMA allocation (invisible until vmaDestroyAllocator asserts) and aliasing the two resources.
    if (destroyed)
    {
        registry_.release_id(id);
    }
    else
    {
        STRING_LOG_WARN("destroy_resource: id {} is not live (double destroy?)", id);
    }
}

// Both of these used bare `at()`, whose "unordered_map::at" locates nothing — and the id IS the
// question on every path that reaches here wrongly: a handle resolved after its backing was freed,
// or an id recycled by a scene switch. Name it.
auto resource_allocator::get_buffer(resource_id id) const -> const allocated_buffer&
{
    const auto it = buffers_.find(id);
    if (it == buffers_.end())
        throw std::out_of_range("resource_allocator::get_buffer: no live buffer with id "
                                + std::to_string(id));
    return it->second;
}

auto resource_allocator::get_image(resource_id id) const -> const allocated_image&
{
    const auto it = images_.find(id);
    if (it == images_.end())
        throw std::out_of_range("resource_allocator::get_image: no live image with id "
                                + std::to_string(id));
    return it->second;
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

// Every image gets a sampler, as it always did — but the configuration now comes from the image's
// own `image_info::sampler` instead of being hardcoded (brief 20). sampler_info's defaults reproduce
// the previous hardcoded configuration, so an image_info that says nothing about sampling gets
// exactly the sampler it got before. A pass that needs NEAREST or CLAMP_TO_EDGE asks for it here
// rather than creating its own VkSampler and forcing it into the descriptor set afterwards.
void resource_allocator::create_image_sampler(allocated_image& allocated_image, const sampler_info& info)
{
    allocated_image.sampler = sampler_for(info);
}

void resource_allocator::set_sampler(resource_id id, const sampler_info& info)
{
    const auto it = images_.find(id);
    if (it == images_.end())
    {
        STRING_LOG_WARN("set_sampler: id {} is not a live image", id);
        return;
    }
    it->second.sampler = sampler_for(info);
}

// One VkSampler per distinct configuration, for the allocator's lifetime. Sharing is what lets
// set_sampler swap an image's sampler with frames in flight: the outgoing handle is still referenced
// by descriptor sets in recorded command buffers, and it stays valid because nobody owns it alone.
auto resource_allocator::sampler_for(const sampler_info& info) -> VkSampler
{
    for (const auto& [cached, sampler] : samplers_)
    {
        if (cached == info)
        {
            return sampler;
        }
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = info.mag_filter,
        .minFilter = info.min_filter,
        .mipmapMode = info.mipmap_mode,
        .addressModeU = info.address_mode,
        .addressModeV = info.address_mode,
        .addressModeW = info.address_mode,
        .mipLodBias = 0.f,
        // maxAnisotropy must be 1.0 when anisotropy is off; the device limit is only legal with it on.
        .anisotropyEnable = info.anisotropy ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = info.anisotropy ? properties.limits.maxSamplerAnisotropy : 1.f,
        .compareEnable = info.compare_enable ? VK_TRUE : VK_FALSE,
        .compareOp = info.compare_op,
        // Non-zero only for a partially-resident image: everything finer than this has no contents.
        .minLod = info.min_lod,
        // Sample across the whole mip chain (trilinear). VK_LOD_CLAMP_NONE lets the hardware pick
        // the level from the derivative regardless of how many levels the image actually has.
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = info.border_color,
        .unnormalizedCoordinates = VK_FALSE
    };

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image sampler!");
    }
    samplers_.emplace_back(info, sampler);
    return sampler;
}

void resource_allocator::create_image_view(allocated_image& allocated_image, VkImageAspectFlags aspect_flags)
{
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = allocated_image.image,
        .viewType = allocated_image.array_layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = allocated_image.format,
        .components = {},
        .subresourceRange = {
            .aspectMask = aspect_flags,
            .baseMipLevel = 0,
            .levelCount = allocated_image.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = allocated_image.array_layers
        }
    };

    if (vkCreateImageView(device_, &view_info, nullptr, &allocated_image.view) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image view!");
    }
}

auto resource_allocator::create_view(resource_id source, const view_range& range) -> resource_id
{
    const allocated_image& src = get_image(source);

    allocated_image slice = src;                 // shares image/allocation/sampler/format/extent
    slice.id = registry_.get_id();
    slice.is_view = true;
    slice.mip_levels = range.mip_count;
    slice.array_layers = range.layer_count;
    slice.view = VK_NULL_HANDLE;

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = src.image,
        .viewType = range.view_type,
        .format = src.format,
        .components = {},
        .subresourceRange = {
            .aspectMask = range.aspect,
            .baseMipLevel = range.base_mip,
            .levelCount = range.mip_count,
            .baseArrayLayer = range.base_layer,
            .layerCount = range.layer_count
        }
    };
    if (vkCreateImageView(device_, &view_info, nullptr, &slice.view) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create sub-resource image view!");
    }

    images_[slice.id] = slice;
    return slice.id;
}

}
