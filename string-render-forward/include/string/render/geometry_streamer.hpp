#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <string/gpu/residency_manager.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/transfer_batch.hpp>

#include <volk.h>

#include <string/render/gltf_types.hpp>

namespace string::render
{

// A minimal first-fit free-list suballocator over a fixed-capacity linear heap (units are elements —
// vertices or indices — not bytes). Allocations carve from a free range; frees are returned sorted
// and coalesced with neighbours. Used to pack streamed per-draw geometry into buffers smaller than
// the whole model, so evicting distant geometry frees space that new geometry reuses.
class HeapSuballocator
{
public:
    explicit HeapSuballocator(std::uint64_t capacity) : capacity_(capacity)
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

// Streams a model's geometry per draw, as a residency_provider driving the residency_manager, with
// real VRAM reclaim: the vertices/indices live in two heaps SMALLER than the whole model, and each
// draw's geometry is suballocated on demand (when the draw enters the view frustum) and freed on
// eviction so its space is reused. Because indices stay global, meshlet vertices hold ORIGINAL global
// indices and the mesh shader rebases them by vertex_offset = (heap vertex offset - the draw's min
// vertex). Gating is free: a not-resident draw stays hidden via DrawInfo.resident 0; the streamer
// flips the resident flag + vertex_offset on residency (and back to 0 on eviction).
//
// Frees are deferred by frames-in-flight: a range evicted at frame N is only reusable at frame
// N + frames_in_flight, after every in-flight frame that could still read it has finished on the GPU.
//
// "detail" is binary: 0 = absent (not drawn), 1 = resident.
class GeometryStreamer final : public ::string::gpu::residency_provider
{
public:
    GeometryStreamer(::string::gpu::resource_allocator& allocator, String::TransferBatch& transfer,
                     ::string::gpu::resource_id vertex_buffer,
                     std::vector<String::Vertex> vertices, std::vector<std::uint32_t> indices,
                     std::uint64_t vertex_capacity,
                     std::uint32_t frames_in_flight);

    // Cooked-scene ctor (brief 04b): no CPU index buffer exists (the cook consumed indices into
    // meshlets); per-draw vertex windows come from set_windows() instead of an index scan.
    GeometryStreamer(::string::gpu::resource_allocator& allocator, String::TransferBatch& transfer,
                     ::string::gpu::resource_id vertex_buffer,
                     std::vector<String::Vertex> vertices,
                     std::uint64_t vertex_capacity,
                     std::uint32_t frames_in_flight);

    void set_draws(const std::vector<GltfDraw>& draws);
    // Cooked path: set per-draw vertex windows directly (offset = min global vertex, count = span).
    // Equivalent to what set_draws() derives from indices, but baked at cook (no runtime index scan).
    struct Window { std::uint32_t offset; std::uint32_t count; };
    void set_windows(const std::vector<Window>& windows);
    std::size_t draw_count() const { return ranges_.size(); }

    // Reclaim ranges whose deferred-free window has elapsed (call once per frame before tick()).
    void begin_frame(std::uint64_t frame);

    // Called when a draw's residency changes, to flip its DrawInfo gate: (draw, vertex_offset,
    // resident). resident == 0 hides the draw (evicted / not yet streamed).
    void set_residency_callback(std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> cb)
    {
        set_cull_ = std::move(cb);
    }

    // Diagnostics.
    std::uint32_t resident_count() const { return resident_draws_; }
    VkDeviceSize streamed_bytes() const { return streamed_bytes_; }
    std::uint32_t evicted_count() const { return evicted_draws_; }

    // residency_provider (id == draw index):
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
        std::uint32_t index_offset = 0;   // first index in the CPU source arrays (meshlet_builder read)
        std::uint32_t index_count = 0;    // (CPU-only; the GPU index heap was removed in brief 04 M3)
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
    // Debug: read a vertex from the CPU-side copy (headless meshlet diagnostics).
    const String::Vertex& cpu_vertex(std::uint32_t i) const { return vertices_[i]; }

private:
    ::string::gpu::resource_allocator& allocator_;
    String::TransferBatch& transfer_;
    ::string::gpu::resource_id vertex_buffer_;
    std::vector<String::Vertex> vertices_;
    std::vector<std::uint32_t> indices_;  // CPU-side, kept for meshlet_builder (no GPU index heap)
    std::vector<DrawRange> ranges_;  // indexed by draw
    std::vector<Alloc> allocs_;      // indexed by draw
    HeapSuballocator vheap_;
    std::vector<PendingFree> pending_free_;
    std::uint32_t frames_in_flight_;
    std::uint64_t current_frame_ = 0;

    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> set_cull_;
    std::uint32_t resident_draws_ = 0;
    std::uint32_t evicted_draws_ = 0;
    VkDeviceSize streamed_bytes_ = 0;
};

}  // namespace string::render
