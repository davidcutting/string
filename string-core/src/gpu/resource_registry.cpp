#include <string/gpu/resource_registry.hpp>

#include <cassert>

#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/resource_allocator.hpp>

namespace string::gpu
{

ResourceRegistry::ResourceRegistry(resource_allocator& allocator, descriptor_table& descriptor_table)
    : allocator_(allocator)
    , descriptor_table_(descriptor_table)
{
}

ResourceRegistry::~ResourceRegistry()
{
    for (buffer_slot& slot : buffers_)
    {
        if (!slot.owned) continue;
        for (resource_id id : slot.physical) allocator_.destroy_resource(id);
    }
    for (image_slot& slot : images_)
    {
        if (!slot.owned) continue;
        for (resource_id id : slot.physical) allocator_.destroy_resource(id);
    }
    transients_.destroy(allocator_);   // free the per-frame scratch arena (brief 16 M5)
}

image ResourceRegistry::import_image(resource_id physical)
{
    const std::uint32_t index = static_cast<std::uint32_t>(images_.size());
    images_.push_back(image_slot{ Lifetime::Imported, { physical }, /*owned=*/false });
    return image{ index };
}

void ResourceRegistry::reimport(image handle, resource_id physical)
{
    assert(handle.valid() && handle.index < images_.size());
    assert(images_[handle.index].lifetime == Lifetime::Imported);
    images_[handle.index].physical = { physical };
}

image ResourceRegistry::create_per_frame_image(const image_info& info, std::uint32_t frames_in_flight)
{
    image_slot slot;
    slot.lifetime = Lifetime::PerFrame;
    slot.owned = true;
    slot.physical.reserve(frames_in_flight);
    for (std::uint32_t f = 0; f < frames_in_flight; ++f)
        slot.physical.push_back(allocator_.create_resource(info));

    const std::uint32_t index = static_cast<std::uint32_t>(images_.size());
    images_.push_back(std::move(slot));
    return image{ index };
}

void ResourceRegistry::recreate_per_frame_image(image handle, const image_info& info)
{
    assert(handle.valid() && handle.index < images_.size());
    image_slot& s = images_[handle.index];
    assert(s.owned && s.lifetime == Lifetime::PerFrame);
    for (resource_id id : s.physical) allocator_.destroy_resource(id);
    for (resource_id& id : s.physical) id = allocator_.create_resource(info);
}

resource_id ResourceRegistry::physical(image handle) const
{
    assert(handle.valid() && handle.index < images_.size());
    return images_[handle.index].physical.front();
}

VkImageView ResourceRegistry::view(image handle) const
{
    return allocator_.get_image(physical(handle)).view;
}

resource_id ResourceRegistry::physical(image handle, std::uint32_t slot) const
{
    assert(handle.valid() && handle.index < images_.size());
    const image_slot& s = images_[handle.index];
    assert(slot < s.physical.size());
    return s.physical[slot];
}

VkImageView ResourceRegistry::view(image handle, std::uint32_t slot) const
{
    return allocator_.get_image(physical(handle, slot)).view;
}

Lifetime ResourceRegistry::lifetime(image handle) const
{
    assert(handle.valid() && handle.index < images_.size());
    return images_[handle.index].lifetime;
}

buffer ResourceRegistry::create_per_frame(const buffer_info& info, std::uint32_t frames_in_flight)
{
    buffer_slot slot;
    slot.lifetime = Lifetime::PerFrame;
    slot.owned = true;
    slot.physical.reserve(frames_in_flight);
    for (std::uint32_t f = 0; f < frames_in_flight; ++f)
        slot.physical.push_back(allocator_.create_resource(info));

    const std::uint32_t index = static_cast<std::uint32_t>(buffers_.size());
    buffers_.push_back(std::move(slot));
    return buffer{ index };
}

void ResourceRegistry::recreate_per_frame(buffer handle, const buffer_info& info)
{
    assert(handle.valid() && handle.index < buffers_.size());
    buffer_slot& b = buffers_[handle.index];
    assert(b.owned && b.lifetime == Lifetime::PerFrame);
    for (resource_id id : b.physical) allocator_.destroy_resource(id);
    for (resource_id& id : b.physical) id = allocator_.create_resource(info);
}

resource_id ResourceRegistry::physical(buffer handle, std::uint32_t slot) const
{
    assert(handle.valid() && handle.index < buffers_.size());
    const buffer_slot& b = buffers_[handle.index];
    assert(slot < b.physical.size());
    return b.physical[slot];
}

VkDeviceAddress ResourceRegistry::address(buffer handle, std::uint32_t slot) const
{
    return allocator_.get_buffer(physical(handle, slot)).device_address;
}

void* ResourceRegistry::mapped(buffer handle, std::uint32_t slot) const
{
    return allocator_.get_buffer(physical(handle, slot)).allocation_info.pMappedData;
}

Lifetime ResourceRegistry::lifetime(buffer handle) const
{
    assert(handle.valid() && handle.index < buffers_.size());
    return buffers_[handle.index].lifetime;
}

}  // namespace string::gpu
