#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/resource_allocator.hpp>

#include <volk.h>

// Brief 16 M7: moved String -> string::gpu (it is a pure GPU-allocator concern; the ResourceRegistry,
// also string::gpu, owns it — brief 16 M5). File kept under string/vulkan/ to avoid include-path churn.
namespace string::gpu
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
//
// Brief 16 M5/M7: intra-frame aliasing between two regions is now driven by GREEDY INTERVAL-PACKING
// over each region's LIFETIME — the half-open pass-index interval [first, last) it is live across.
// reserve() places a region at the lowest byte offset not occupied by any TIME-OVERLAPPING region,
// so two regions whose lifetimes DON'T overlap share memory. Default lifetime = the whole frame
// [0, MAX), so today's regions (all live across the geometry pass) overlap and pack disjoint —
// byte-identical to the old bump allocator until a caller supplies real lifetimes.
struct ScratchLifetime
{
    uint32_t first = 0;               // first pass index the region is live (inclusive)
    uint32_t last = ~uint32_t{ 0 };   // last pass index the region is live (exclusive upper bound)
    bool overlaps(const ScratchLifetime& o) const { return first < o.last && o.first < last; }
};

class FrameScratch
{
public:
    // During pass construction: reserve `bytes` of per-frame scratch. Returns the byte offset of
    // the region (identical in every slot's buffer). 256-byte aligned by default (covers SSBO /
    // indirect / device-address alignment requirements). `life` = the pass-index interval the region
    // is live across; regions with non-overlapping lifetimes alias the same memory (default = whole
    // frame, i.e. no aliasing — byte-identical to the pre-M5 bump allocator).
    VkDeviceSize reserve(VkDeviceSize bytes, VkDeviceSize alignment = 256, ScratchLifetime life = {});

    // After all passes are built: create one device-local buffer per frame slot sized to the
    // total reservation. No-op if nothing was reserved.
    void materialize(string::gpu::resource_allocator& allocator, uint32_t frame_slots);
    void destroy(string::gpu::resource_allocator& allocator);

    bool materialized() const { return !buffers_.empty(); }
    VkDeviceSize size() const { return size_; }
    string::gpu::resource_id buffer(uint32_t slot) const { return buffers_[slot]; }
    VkDeviceAddress address(uint32_t slot) const { return addresses_[slot]; }

private:
    struct Region
    {
        VkDeviceSize offset;
        VkDeviceSize size;
        ScratchLifetime life;
    };
    VkDeviceSize size_ = 0;
    std::vector<Region> regions_;   // placed regions, for interval-packing subsequent reserves
    std::vector<string::gpu::resource_id> buffers_;
    std::vector<VkDeviceAddress> addresses_;
};

}  // namespace string::gpu
