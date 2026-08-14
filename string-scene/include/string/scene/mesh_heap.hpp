#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <string/gpu/residency_manager.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/core/vertex.hpp>
#include <string/vulkan/transfer_batch.hpp>

#include <volk.h>

namespace string::assets
{

// A minimal first-fit free-list suballocator over a fixed-capacity linear heap (units are elements —
// vertices — not bytes). Allocations carve from a free range; frees are returned sorted and
// coalesced with neighbours. Used to pack streamed per-mesh geometry into a buffer smaller than the
// whole content set, so evicting distant geometry frees space that new geometry reuses.
class heap_suballocator
{
public:
    explicit heap_suballocator(std::uint64_t capacity) : capacity_(capacity)
    {
        if (capacity_ > 0)
        {
            free_.push_back({ 0, capacity_ });
        }
    }

    bool can_allocate(std::uint64_t size) const
    {
        if (size == 0) return true;
        for (const Range& r : free_)
        {
            if (r.size >= size) return true;
        }
        return false;
    }

    std::optional<std::uint64_t> allocate(std::uint64_t size)
    {
        if (size == 0) return std::uint64_t{ 0 };
        for (std::size_t i = 0; i < free_.size(); ++i)
        {
            if (free_[i].size >= size)
            {
                const std::uint64_t offset = free_[i].offset;
                free_[i].offset += size;
                free_[i].size -= size;
                if (free_[i].size == 0) free_.erase(free_.begin() + i);
                return offset;
            }
        }
        return std::nullopt;
    }

    void free(std::uint64_t offset, std::uint64_t size)
    {
        if (size == 0) return;
        std::size_t i = 0;
        while (i < free_.size() && free_[i].offset < offset) ++i;
        free_.insert(free_.begin() + i, { offset, size });
        // Coalesce with the following then the preceding range if adjacent.
        if (i + 1 < free_.size() && free_[i].offset + free_[i].size == free_[i + 1].offset)
        {
            free_[i].size += free_[i + 1].size;
            free_.erase(free_.begin() + i + 1);
        }
        if (i > 0 && free_[i - 1].offset + free_[i - 1].size == free_[i].offset)
        {
            free_[i - 1].size += free_[i].size;
            free_.erase(free_.begin() + i);
        }
    }

    std::uint64_t capacity() const { return capacity_; }

private:
    struct Range { std::uint64_t offset; std::uint64_t size; };
    std::uint64_t capacity_;
    std::vector<Range> free_;
};

// The asset registry's vertex heap: streams content geometry per mesh part, as a residency_provider
// driving the residency_manager, with real VRAM reclaim. The vertices live in a heap SMALLER than
// (or equal to) the whole content set, and each part's geometry is suballocated on demand (when it
// enters the view frustum) and freed on eviction so its space is reused. Because meshlet vertices
// hold ORIGINAL global indices, the mesh shader rebases them by vertex_offset = (heap vertex offset
// - the part's min vertex). Gating is free: a not-resident part stays hidden via its DrawInfo
// resident flag; the residency callback flips resident + vertex_offset (the renderer installs it —
// the DrawInfo table is a renderer mirror the registry never sees).
//
// Frees are deferred by frames-in-flight: a range evicted at frame N is only reusable at frame
// N + frames_in_flight, after every in-flight frame that could still read it has finished.
//
// "detail" is binary: 0 = absent (not drawn), 1 = resident.
// (Moved from string-render-forward's GeometryStreamer with the asset-layer split.)
class mesh_heap final : public ::string::gpu::residency_provider
{
public:
    // `vertices` is a NON-OWNING view of the registry's CPU vertex table (the registry owns both
    // this object and the table, so the lifetime is internal).
    mesh_heap(::string::gpu::resource_allocator& allocator, string::TransferBatch& transfer,
              ::string::gpu::resource_id vertex_buffer, std::span<const string::Vertex> vertices,
              std::uint64_t vertex_capacity, std::uint32_t frames_in_flight);

    // Set per-part vertex windows (offset = min global vertex, count = span), baked at cook.
    struct Window { std::uint32_t offset; std::uint32_t count; };
    void set_windows(const std::vector<Window>& windows);
    std::size_t draw_count() const { return ranges_.size(); }

    // Reclaim ranges whose deferred-free window has elapsed (call once per frame before tick()).
    void begin_frame(std::uint64_t frame);

    // Called when a part's residency changes, to flip its DrawInfo gate: (part, vertex_offset,
    // resident). resident == 0 hides the part (evicted / not yet streamed).
    void set_residency_callback(std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> cb)
    {
        set_cull_ = std::move(cb);
    }

    // Diagnostics.
    std::uint32_t resident_count() const { return resident_draws_; }
    VkDeviceSize streamed_bytes() const { return streamed_bytes_; }
    std::uint32_t evicted_count() const { return evicted_draws_; }

    // residency_provider (id == mesh-part index — its own id space, distinct from texture
    // resource_ids because it lives in its own residency_manager; never merge the two managers):
    std::uint64_t stream(::string::gpu::resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) override;
    bool is_complete(std::uint64_t ticket) override;
    void on_resident(::string::gpu::resource_id id, std::uint32_t detail) override;
    void evict(::string::gpu::resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) override;
    VkDeviceSize cost(::string::gpu::resource_id id, std::uint32_t detail) override;
    bool can_stream(::string::gpu::resource_id id, std::uint32_t to_detail) override;

private:
    struct DrawRange
    {
        std::uint32_t vertex_offset = 0;  // min index (first vertex) in the source arrays
        std::uint32_t vertex_count = 0;
        std::uint32_t index_offset = 0;   // legacy field, unused on the cooked path
        std::uint32_t index_count = 0;    // non-zero => resident-eligible (stream()'s empty check)
    };
    struct Alloc
    {
        std::uint64_t vheap = 0;  // heap vertex offset (valid while resident)
        bool resident = false;
    };
    struct PendingFree
    {
        std::uint64_t vheap, vcount, safe_frame;
    };

public:
    // Debug: read a vertex from the CPU-side table (headless meshlet diagnostics).
    const string::Vertex& cpu_vertex(std::uint32_t i) const { return vertices_[i]; }

private:
    ::string::gpu::resource_allocator& allocator_;
    string::TransferBatch& transfer_;
    ::string::gpu::resource_id vertex_buffer_;
    std::span<const string::Vertex> vertices_;   // registry-owned; non-owning view
    std::vector<DrawRange> ranges_;  // indexed by part
    std::vector<Alloc> allocs_;      // indexed by part
    heap_suballocator vheap_;
    std::vector<PendingFree> pending_free_;
    std::uint32_t frames_in_flight_;
    std::uint64_t current_frame_ = 0;

    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> set_cull_;
    std::uint32_t resident_draws_ = 0;
    std::uint32_t evicted_draws_ = 0;
    VkDeviceSize streamed_bytes_ = 0;
};

}  // namespace string::assets
