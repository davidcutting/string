#include <string/vulkan/descriptor_set_allocator.hpp>
#include <string/vulkan/device.hpp>
#include <vector>
#include <stdexcept>

namespace String {

DescriptorAllocator::DescriptorAllocator(std::shared_ptr<Device> device)
: device_(std::move(device))
{
    // Create descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = MAX_BINDLESS_TEXTURES,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = MAX_BINDLESS_BUFFERS,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr
        }
    };

    VkDescriptorBindingFlags binding_flags[] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
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
        .flags = 0,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };

    if (vkCreateDescriptorSetLayout(device_->get_device(), &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor set layout");
    }

    // Create descriptor pool
    std::vector<VkDescriptorPoolSize> pool_sizes = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_BINDLESS_TEXTURES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_BINDLESS_BUFFERS }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data()
    };

    if (vkCreateDescriptorPool(device_->get_device(), &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bindless descriptor pool");
    }

    // Allocate descriptor set
    uint32_t max_descriptors[] = { MAX_BINDLESS_TEXTURES, MAX_BINDLESS_BUFFERS };

    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorSetCount = 1,
        .pDescriptorCounts = max_descriptors
    };

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variable_count_info,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout_
    };

    if (vkAllocateDescriptorSets(device_->get_device(), &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate bindless descriptor set");
    }
}

DescriptorAllocator::~DescriptorAllocator() {
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(device_->get_device(), descriptor_pool_, nullptr);
    }
    if (descriptor_set_layout_) {
        vkDestroyDescriptorSetLayout(device_->get_device(), descriptor_set_layout_, nullptr);
    }
}

auto DescriptorAllocator::allocate_texture(VkImageView view, VkSampler sampler) -> uint32_t {
    if (next_texture_index_ >= MAX_BINDLESS_TEXTURES) {
        throw std::runtime_error("Exceeded maximum bindless textures");
    }

    VkDescriptorImageInfo image_info = {
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet image_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = 0,
        .dstArrayElement = next_texture_index_,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr
    };

    vkUpdateDescriptorSets(device_->get_device(), 1, &image_write, 0, nullptr);

    return next_texture_index_++;
}

auto DescriptorAllocator::allocate_buffer(VkBuffer buffer, VkDeviceSize size) -> uint32_t {
    if (next_buffer_index_ >= MAX_BINDLESS_BUFFERS) {
        throw std::runtime_error("Exceeded maximum bindless buffers");
    }

    VkDescriptorBufferInfo buffer_info = {
        .buffer = buffer,
        .offset = 0,
        .range = size
    };

    VkWriteDescriptorSet buffer_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_,
        .dstBinding = 0,
        .dstArrayElement = next_texture_index_,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = nullptr,
        .pBufferInfo = &buffer_info,
        .pTexelBufferView = nullptr
    };

    vkUpdateDescriptorSets(device_->get_device(), 1, &buffer_write, 0, nullptr);

    return next_buffer_index_++;
}

} // namespace String