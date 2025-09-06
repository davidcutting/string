#pragma once

#include <cstdint>

#include <string/vulkan/resource_allocator.hpp>

#include <volk.h>

namespace String
{

class Device;

class DescriptorAllocator
{
    VkDevice& device_;
    ResourceAllocator& allocator_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    uint32_t next_texture_index_ = 0;
    uint32_t next_buffer_index_ = 0;

    static constexpr uint32_t MAX_BINDLESS_TEXTURES = 65536;
    static constexpr uint32_t MAX_BINDLESS_BUFFERS  = 16384;
    static constexpr uint32_t TEXTURE_BINDING_SLOT = 0;
    static constexpr uint32_t BUFFER_BINDING_SLOT  = 1;

public:
    explicit DescriptorAllocator(VkDevice& device, ResourceAllocator& allocator);
    ~DescriptorAllocator();

    void allocate_image(const ResourceID& handle);
    void allocate_buffer(const ResourceID& handle);

    auto get_descriptor_set_layout() const -> VkDescriptorSetLayout { return descriptor_set_layout_; }
    auto get_descriptor_set() const -> VkDescriptorSet { return descriptor_set_; }
};

} // namespace String