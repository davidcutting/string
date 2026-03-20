#pragma once

#include <cstdint>
#include <unordered_map>

#include <string/vulkan/resource.hpp>
#include <string/vulkan/resource_allocator.hpp>

#include <volk.h>

namespace String
{

enum class DescriptorType
{
    STORAGE_BUFFER,
    TEXTURE,
    STORAGE_IMAGE,
    TLAS,
    BLAS,
};

using Slot = std::uint32_t;

class DescriptorAllocator
{
    IDRegistry registry_;
    std::unordered_map<ResourceID, Slot> slot_map_;

public:
    auto allocate(const ResourceID& resource) -> Slot
    {
        const Slot slot = registry_.get_id();
        slot_map_[resource] = slot;
        return slot;
    }

    void release(const ResourceID& resource)
    {
        registry_.release_id(get_slot(resource));
        slot_map_.erase(resource);
    }

    auto get_slot(const ResourceID& resource) const -> Slot
    {
        return slot_map_.at(resource);
    }
};

struct SlotRelease
{
    Slot slot;
    ResourceID resource;
    DescriptorType type;
};

class DescriptorTable
{
    VkDevice& device_;
    ResourceAllocator& allocator_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    DescriptorAllocator ssbo_descriptor_allocator_;
    DescriptorAllocator texture_descriptor_allocator_;
    DescriptorAllocator st_image_descriptor_allocator_;
    DescriptorAllocator tlas_descriptor_allocator_;
    DescriptorAllocator blas_descriptor_allocator_;

    static constexpr uint32_t MAX_BINDLESS_IMAGES   = 65536;
    static constexpr uint32_t MAX_BINDLESS_BUFFERS  = 16384;
    static constexpr uint32_t MAX_BINDLESS_ACCEL_STRUCT = 1024;

public:
    explicit DescriptorTable(VkDevice& device, ResourceAllocator& allocator);
    ~DescriptorTable();

    void bind(const ResourceID& handle, const DescriptorType& type);
    void unbind(const ResourceID& handle, const DescriptorType& type);

    auto get_binding_slot(const ResourceID& handle, const DescriptorType& type) -> std::uint32_t;
    auto get_layout() const -> VkDescriptorSetLayout { return descriptor_set_layout_; }
    auto get_set() const -> VkDescriptorSet { return descriptor_set_; }

private:
    void bind_buffer(const ResourceID& handle, const VkDescriptorType& type, const uint32_t& slot);
    void bind_image(const ResourceID& handle, const VkDescriptorType& type, const uint32_t& slot);
};

} // namespace String