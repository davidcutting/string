#include <algorithm>
#include <stdexcept>
#include <vector>

#include <string/core/logger.hpp>
#include <string/vulkan/frame_scratch.hpp>

namespace string::gpu
{

VkDeviceSize FrameScratch::reserve(VkDeviceSize bytes, VkDeviceSize alignment, ScratchLifetime life)
{
    if (materialized())
        throw std::runtime_error("FrameScratch: reserve() after materialize()");

    // Greedy interval-packing: place this region at the LOWEST aligned offset whose byte range
    // [offset, offset+bytes) does not collide with any already-placed region whose LIFETIME overlaps
    // this one's. Candidate offsets are 0 and the aligned end of every time-overlapping region — a
    // classic first-fit over the union of "busy" byte intervals (only those live at the same time).
    // When every region shares the default whole-frame lifetime, they all overlap, so the lowest
    // non-colliding offset is always past all of them == the old bump allocator (byte-identical).
    const auto align_up = [alignment](VkDeviceSize v) { return (v + alignment - 1) & ~(alignment - 1); };

    std::vector<VkDeviceSize> candidates{ 0 };
    for (const Region& r : regions_)
        if (r.life.overlaps(life)) candidates.push_back(align_up(r.offset + r.size));
    std::sort(candidates.begin(), candidates.end());

    VkDeviceSize offset = 0;
    for (const VkDeviceSize cand : candidates)
    {
        const VkDeviceSize start = align_up(cand);
        bool collides = false;
        for (const Region& r : regions_)
        {
            if (!r.life.overlaps(life)) continue;
            // byte-range overlap of [start, start+bytes) vs [r.offset, r.offset+r.size)
            if (start < r.offset + r.size && r.offset < start + bytes) { collides = true; break; }
        }
        if (!collides) { offset = start; break; }
    }

    regions_.push_back({ offset, bytes, life });
    size_ = std::max(size_, offset + bytes);
    return offset;
}

void FrameScratch::materialize(string::gpu::resource_allocator& allocator, uint32_t frame_slots)
{
    if (size_ == 0 || materialized()) return;
    buffers_.reserve(frame_slots);
    addresses_.reserve(frame_slots);
    for (uint32_t slot = 0; slot < frame_slots; ++slot)
    {
        const string::gpu::resource_id id = allocator.create_resource(string::gpu::buffer_info{
            .size = size_,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        buffers_.push_back(id);
        addresses_.push_back(allocator.get_buffer(id).device_address);
    }
    STRING_LOG_INFO("[scratch] {} KiB per frame slot x {} slots ({} KiB total)",
                    size_ / 1024, frame_slots, size_ * frame_slots / 1024);
}

void FrameScratch::destroy(string::gpu::resource_allocator& allocator)
{
    for (string::gpu::resource_id id : buffers_)
        allocator.destroy_resource(id);
    buffers_.clear();
    addresses_.clear();
    regions_.clear();
    size_ = 0;
}

}  // namespace string::gpu
