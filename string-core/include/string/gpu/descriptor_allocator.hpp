#pragma once

#include <stdexcept>
#include <string>
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
    // A resource's slot is STABLE for as long as it stays bound: re-allocating an already-bound
    // resource returns the slot it already has, so bind() rewrites that slot's descriptor in place.
    //
    // Minting a fresh slot instead is silent corruption, because a slot index is DATA the GPU already
    // holds — material tables, SceneData, every shader-side handle stores the number returned the
    // first time. A second bind() would move the resource to a slot nothing references and strand the
    // referenced one holding whatever the FIRST bind wrote (for a streamed texture: the image before
    // a single mip had been uploaded). Every streamed texture in the scene sampled that stale
    // descriptor for the rest of the run, which is what flattened Sponza's materials to single
    // colours. It also leaked a slot per rebind. bind() is the design's rebinding verb (brief 20);
    // this is what makes it one.
    auto allocate(resource_id resource) -> slot
    {
        const auto it = slot_map_.find(resource);
        if (it != slot_map_.end())
        {
            return it->second;
        }
        const slot slot = registry_.get_id();
        slot_map_[resource] = slot;
        return slot;
    }

    // Releasing something that was never bound is a NO-OP, not an error. This is teardown: a caller
    // freeing a resource it owns should not first have to know whether that resource ever reached a
    // descriptor slot. The streamer is the case that proves it — a texture is created, then bound
    // when its first mip lands, so a scene unloaded mid-stream holds images that legitimately have
    // no slot, and `at()` threw std::out_of_range on the first scene switch away from Sponza.
    //
    // `get_slot()` stays strict: reading the slot of an unbound resource is a genuine bug at the
    // point of use, where the throw actually locates it.
    void release(resource_id resource)
    {
        const auto it = slot_map_.find(resource);
        if (it == slot_map_.end()) return;
        registry_.release_id(it->second);
        slot_map_.erase(it);
    }

    auto get_slot(resource_id resource) const -> slot
    {
        const auto it = slot_map_.find(resource);
        // `slot_map_.at()` threw "unordered_map::at", which locates nothing — and this fires on the
        // paths where the resource id is the whole question (a scene switch recycling ids, a
        // resource used before it was bound). Name the id and how many are bound.
        if (it == slot_map_.end())
        {
            throw std::out_of_range("descriptor_allocator::get_slot: resource id "
                                    + std::to_string(resource) + " is not bound ("
                                    + std::to_string(slot_map_.size()) + " bound)");
        }
        return it->second;
    }

    // get_slot() throws on an unbound handle; unbind() is called speculatively in destructors, so
    // it needs to ask first.
    [[nodiscard]] bool has(resource_id resource) const
    {
        return slot_map_.find(resource) != slot_map_.end();
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

    // Write a VK_NULL_HANDLE image descriptor into `slot`, nulling it out.
    //
    // Releasing a slot used to free only the INDEX, leaving the descriptor set still holding the
    // VkImageView/VkSampler of a resource that was about to be destroyed. Those handles stay in the
    // set until something rebinds that slot — so after a scene teardown the set can carry hundreds
    // of dangling image descriptors, and binding a set with descriptors referencing destroyed
    // objects is undefined behaviour. It happens to survive on some drivers and faults the GPU on
    // others, which is exactly the sort of bug that only shows up on someone else's machine.
    //
    // Legal because VK_EXT_robustness2's nullDescriptor is a hard device requirement (device.hpp):
    // reads return zero, writes are discarded. No resource, no lifetime, no layout.
    void write_null(uint32_t slot, VkDescriptorType type);

    // Write a resource's descriptor into an already-allocated slot. bind() is the only caller:
    // bind_image reads the sampler straight off the resource (`allocated_image::sampler`, configured
    // per-image through `image_info::sampler`), which is what makes bind() sufficient on its own.
    void bind_buffer(resource_id handle, VkDescriptorType type, uint32_t slot);
    void bind_image(resource_id handle, VkDescriptorType type, uint32_t slot);

    // Kept solely so a nulled COMBINED_IMAGE_SAMPLER slot has the valid sampler the spec still
    // demands alongside a null image view. Created on first use, destroyed with the table.
    VkSampler null_sampler_ = VK_NULL_HANDLE;

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

    auto get_binding_slot(resource_id handle, descriptor_type type) -> std::uint32_t;
    auto get_layout() const -> VkDescriptorSetLayout { return descriptor_set_layout_; }
    auto get_set() const -> VkDescriptorSet { return descriptor_set_; }
};

} // namespace string::gpu