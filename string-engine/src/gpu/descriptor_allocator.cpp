#include <cstdint>
#include <stdexcept>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <vector>
#include <string/gpu/resource_allocator.hpp>

namespace string::gpu
{

descriptor_table::descriptor_table(VkDevice device, resource_allocator& allocator)
: device_(device)
, allocator_(allocator)
{
    // Create descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = MAX_BINDLESS_BUFFERS,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = MAX_BINDLESS_IMAGES,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = MAX_BINDLESS_IMAGES,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr
        },
        // {
        //     .binding = 3,
        //     .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        //     .descriptorCount = MAX_BINDLESS_ACCEL_STRUCT,
        //     .stageFlags = VK_SHADER_STAGE_ALL,
        //     .pImmutableSamplers = nullptr
        // },
        // {
        //     .binding = 4,
        //     .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        //     .descriptorCount = MAX_BINDLESS_ACCEL_STRUCT,
        //     .stageFlags = VK_SHADER_STAGE_ALL,
        //     .pImmutableSamplers = nullptr
        // },
    };

    // VARIABLE_DESCRIPTOR_COUNT_BIT must be on the highest-numbered binding, so it lives on
    // the last binding (storage images), whose descriptorCount matches the variable count max.
    VkDescriptorBindingFlags binding_flags[] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, // binding 0: fixed size ssbos
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, // binding 1: combined image samplers
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, // binding 2: storage images (last)
        // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, // fixed size tlas
        // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, // fixed size blas
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = nullptr,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindingFlags = binding_flags
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &binding_flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create bindless descriptor set layout");
    }

    // Create descriptor pool
    std::vector<VkDescriptorPoolSize> pool_sizes = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_BINDLESS_BUFFERS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_BINDLESS_IMAGES },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_BINDLESS_IMAGES },
        // { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_BINDLESS_ACCEL_STRUCT },
        // { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_BINDLESS_ACCEL_STRUCT },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data()
    };

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create bindless descriptor pool");
    }

    // Allocate descriptor set
    // Currently we can limit size of everything to the number of images, since UBOs and SSBOs are fixed
    std::array<uint32_t, 1> max_descriptors = { MAX_BINDLESS_IMAGES };

    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorSetCount = 1,
        .pDescriptorCounts = max_descriptors.data()
    };

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variable_count_info,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout_
    };

    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate bindless descriptor set");
    }
}

descriptor_table::~descriptor_table()
{
    if (descriptor_pool_)
    {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    if (descriptor_set_layout_)
    {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    }
}

void descriptor_table::bind(resource_id handle, descriptor_type type)
{
    uint32_t slot = 0;

    switch(type)
    {
        case descriptor_type::STORAGE_BUFFER:
            slot = ssbo_descriptor_allocator_.allocate(handle);
            bind_buffer(handle, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slot);
            break;

        case descriptor_type::TEXTURE:
            slot = texture_descriptor_allocator_.allocate(handle);
            bind_image(handle, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slot);
            break;

        case descriptor_type::STORAGE_IMAGE:
            slot = st_image_descriptor_allocator_.allocate(handle);
            bind_image(handle, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slot);
            break;
        
        case descriptor_type::TLAS:
            slot = tlas_descriptor_allocator_.allocate(handle);
            // bind_image(handle, VK_DESCRIPTOR_TYPE_SAMPLER, slot);
            break;
        
        case descriptor_type::BLAS:
            slot = blas_descriptor_allocator_.allocate(handle);
            // bind_image(handle, VK_DESCRIPTOR_TYPE_SAMPLER, slot);
            break;
    }
}

void descriptor_table::unbind(resource_id handle, descriptor_type type)
{
    // TODO(DCut): Handle timeline value and flushing
    switch(type)
    {
        case descriptor_type::STORAGE_BUFFER:
            ssbo_descriptor_allocator_.release(handle);
            break;
        case descriptor_type::TEXTURE:
            texture_descriptor_allocator_.release(handle);
            break;
        case descriptor_type::STORAGE_IMAGE:
            st_image_descriptor_allocator_.release(handle);
            break;
        case descriptor_type::TLAS:
            tlas_descriptor_allocator_.release(handle);
            break;
        case descriptor_type::BLAS:
            tlas_descriptor_allocator_.release(handle);
            break;
    }
}

auto descriptor_table::get_binding_slot(resource_id handle, descriptor_type type) -> std::uint32_t
{
    switch(type)
    {
        case descriptor_type::STORAGE_BUFFER:    return ssbo_descriptor_allocator_.get_slot(handle);
        case descriptor_type::TEXTURE:           return texture_descriptor_allocator_.get_slot(handle);
        case descriptor_type::STORAGE_IMAGE:     return st_image_descriptor_allocator_.get_slot(handle);
        case descriptor_type::TLAS:              return tlas_descriptor_allocator_.get_slot(handle);
        case descriptor_type::BLAS:              return blas_descriptor_allocator_.get_slot(handle);
    }
    throw std::runtime_error("Failed to get binding slot, somehow hit unreachable code.");
}

void descriptor_table::bind_buffer(resource_id handle, VkDescriptorType type, uint32_t slot)
{
    const auto& buffer = allocator_.get_buffer(handle);

    VkDescriptorBufferInfo buffer_info = {
        .buffer = buffer.buffer,
        .offset = 0,
        .range  = buffer.size
    };

    // Layout binding 0 is the storage-buffer array (the only buffer binding in the table).
    const uint32_t dst_binding = 0;

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = dst_binding,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = nullptr,
        .pBufferInfo = &buffer_info,
        .pTexelBufferView = nullptr
    };

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void descriptor_table::update_texture(std::uint32_t slot, VkImageView view, VkSampler sampler)
{
    const VkDescriptorImageInfo image_info = {
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = 1,  // combined image samplers (textures)
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

auto descriptor_table::bind_storage_view(VkImageView view) -> std::uint32_t
{
    // Synthetic keys above the real resource-id range so the storage-image slot allocator can track
    // per-mip HiZ views (one image, several slots) without colliding with resource-backed binds.
    static resource_id synthetic_key = 0xF0000000u;
    const resource_id key = synthetic_key++;
    const uint32_t slot = st_image_descriptor_allocator_.allocate(key);

    const VkDescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = 2,  // storage images
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    slot_to_synthetic_[slot] = key;
    return slot;
}

void descriptor_table::unbind_storage_view(std::uint32_t slot)
{
    auto it = slot_to_synthetic_.find(slot);
    if (it == slot_to_synthetic_.end()) return;
    st_image_descriptor_allocator_.release(it->second);
    slot_to_synthetic_.erase(it);
}

void descriptor_table::bind_image(resource_id handle, VkDescriptorType type, uint32_t slot)
{
    const auto& image = allocator_.get_image(handle);

    const auto image_layout = (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ?
                                VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo image_info = {
        .sampler = image.sampler,
        .imageView = image.view,
        .imageLayout = image_layout, 
    };

    // Layout binding 1 is combined image samplers (textures), binding 2 is storage images.
    const uint32_t dst_binding = (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ? 1 : 2;

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = dst_binding,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr
    };

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

} // namespace string::gpu