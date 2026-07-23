#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/resource_allocator.hpp>

#include <volk.h>

namespace String
{

// Brief 04e M3: per-frame-slot GPU scratch arena. Passes RESERVE their per-frame transient byte
// needs during construction (worklists, counts, compacted command lists — anything rebuilt from
// scratch every frame); the renderer MATERIALIZES one device-local buffer per frame slot after
// all passes are built, and each pass addresses its region as buffer(slot) + its offset.
//
// This is also the transient-ALIASING arena: a region's contents are garbage at frame start by
// definition (rule: acquire from UNDEFINED — nothing may read a scratch region before writing
// it this frame), successive frames of the SAME slot alias trivially, and frames in flight
// never share a placement because each slot has its own buffer (the 04d frames-in-flight rule).
// Intra-frame aliasing between two regions requires their graph lifetimes not to overlap —
// the planner's resource_lifetimes is the input; today's frame has no non-overlapping pairs
// (everything spans the geometry pass), so regions are simply packed disjoint.
class FrameScratch
{
public:
    // During pass construction: reserve `bytes` of per-frame scratch. Returns the byte offset of
    // the region (identical in every slot's buffer). 256-byte aligned by default (covers SSBO /
    // indirect / device-address alignment requirements).
    VkDeviceSize reserve(VkDeviceSize bytes, VkDeviceSize alignment = 256);

    // After all passes are built: create one device-local buffer per frame slot sized to the
    // total reservation. No-op if nothing was reserved.
    void materialize(string::gpu::resource_allocator& allocator, uint32_t frame_slots);
    void destroy(string::gpu::resource_allocator& allocator);

    bool materialized() const { return !buffers_.empty(); }
    VkDeviceSize size() const { return size_; }
    string::gpu::resource_id buffer(uint32_t slot) const { return buffers_[slot]; }
    VkDeviceAddress address(uint32_t slot) const { return addresses_[slot]; }

private:
    VkDeviceSize size_ = 0;
    std::vector<string::gpu::resource_id> buffers_;
    std::vector<VkDeviceAddress> addresses_;
};

}  // namespace String
