#pragma once

#include <cstdint>
#include <unordered_map>

#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>

#include <volk.h>

namespace string::gpu
{

enum class descriptor_type
{
    STORAGE_BUFFER,
    TEXTURE,
    STORAGE_IMAGE,
    TLAS,
    BLAS,
};

using slot = std::uint32_t;

class descriptor_allocator
{
    id_registry registry_;
    std::unordered_map<resource_id, slot> slot_map_;

public:
    auto allocate(resource_id resource) -> slot
    {
        const slot slot = registry_.get_id();
        slot_map_[resource] = slot;
        return slot;
    }

    void release(resource_id resource)
    {
        registry_.release_id(get_slot(resource));
        slot_map_.erase(resource);
    }

    auto get_slot(resource_id resource) const -> slot
    {
        return slot_map_.at(resource);
    }
};

struct slot_release
{
    slot slot;
    resource_id resource;
    descriptor_type type;
};

class descriptor_table
{
    VkDevice device_;
    resource_allocator& allocator_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    descriptor_allocator ssbo_descriptor_allocator_;
    descriptor_allocator texture_descriptor_allocator_;
    descriptor_allocator st_image_descriptor_allocator_;
    descriptor_allocator tlas_descriptor_allocator_;
    descriptor_allocator blas_descriptor_allocator_;

public:
    // Advertised bindless capacities. Public so device suitability can verify the physical
    // device's update-after-bind limits are at least this large before selecting it.
    static constexpr uint32_t MAX_BINDLESS_IMAGES   = 65536;
    static constexpr uint32_t MAX_BINDLESS_BUFFERS  = 16384;
    static constexpr uint32_t MAX_BINDLESS_ACCEL_STRUCT = 1024;

    explicit descriptor_table(VkDevice device, resource_allocator& allocator);
    ~descriptor_table();

    void bind(resource_id handle, descriptor_type type);
    void unbind(resource_id handle, descriptor_type type);

    // Rewrite an already-allocated texture slot's combined-image-sampler (binding 1) with a new
    // view/sampler. Used by streaming to swap a texture's sampler as its resident mip range grows
    // or shrinks (an adjustable minLod), without reallocating the bindless slot. The set is
    // UPDATE_AFTER_BIND, so this is safe to call between frames while the slot stays bound.
    void update_texture(std::uint32_t slot, VkImageView view, VkSampler sampler);

    // Bind an arbitrary (caller-owned) image view into a fresh storage-image slot (binding 2), with
    // no backing resource_id. Used for per-mip views of a HiZ pyramid where one image needs several
    // storage slots (the resource-keyed bind() can't give more than one slot per handle). The view's
    // lifetime is the caller's; unbind_storage_view() frees the slot. Returns the slot.
    auto bind_storage_view(VkImageView view) -> std::uint32_t;
    void unbind_storage_view(std::uint32_t slot);

    auto get_binding_slot(resource_id handle, descriptor_type type) -> std::uint32_t;
    auto get_layout() const -> VkDescriptorSetLayout { return descriptor_set_layout_; }
    auto get_set() const -> VkDescriptorSet { return descriptor_set_; }

private:
    void bind_buffer(resource_id handle, VkDescriptorType type, uint32_t slot);
    void bind_image(resource_id handle, VkDescriptorType type, uint32_t slot);

    // Maps a storage-image slot from bind_storage_view() back to its synthetic allocator key, so
    // unbind_storage_view() can release it (these slots have no resource_id).
    std::unordered_map<std::uint32_t, resource_id> slot_to_synthetic_;
};

} // namespace string::gpu