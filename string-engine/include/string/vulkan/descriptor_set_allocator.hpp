#pragma once

#include <volk.h>
#include <cstdint>
#include <memory>

namespace String
{

class Device;

class DescriptorAllocator
{
    std::shared_ptr<Device> device_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    uint32_t next_texture_index_ = 0;
    uint32_t next_buffer_index_ = 0;

public:
    static constexpr uint32_t MAX_BINDLESS_TEXTURES = 65536;
    static constexpr uint32_t MAX_BINDLESS_BUFFERS  = 16384;

    explicit DescriptorAllocator(std::shared_ptr<Device> device);
    ~DescriptorAllocator();

    auto allocate_texture(VkImageView view, VkSampler sampler) -> uint32_t;
    auto allocate_buffer(VkBuffer buffer, VkDeviceSize size) -> uint32_t;

    auto get_descriptor_set_layout() const -> VkDescriptorSetLayout { return descriptor_set_layout_; }
    auto get_descriptor_set() const -> VkDescriptorSet { return descriptor_set_; }
};

} // namespace String