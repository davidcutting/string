#include <algorithm>
#include <limits>

#include <string/scene/mesh_heap.hpp>

namespace string::assets
{
using namespace string;

mesh_heap::mesh_heap(::string::gpu::resource_allocator& allocator, TransferBatch& transfer,
                                   ::string::gpu::resource_id vertex_buffer,
                                   std::span<const Vertex> vertices,
                                   std::uint64_t vertex_capacity,
                                   std::uint32_t frames_in_flight)
: allocator_(allocator)
, transfer_(transfer)
, vertex_buffer_(vertex_buffer)
, vertices_(vertices)
, vheap_(vertex_capacity)
, frames_in_flight_(frames_in_flight)
{
    (void)allocator_;  // held for symmetry with other providers
}

void mesh_heap::set_windows(const std::vector<Window>& windows)
{
    ranges_.resize(windows.size());
    allocs_.assign(windows.size(), Alloc{});
    for (std::size_t i = 0; i < windows.size(); ++i)
    {
        DrawRange r;
        r.vertex_offset = windows[i].offset;
        r.vertex_count = windows[i].count;
        r.index_offset = 0;               // no CPU index buffer in the cooked path
        r.index_count = windows[i].count; // non-zero => resident-eligible (stream()'s empty check)
        ranges_[i] = r;
    }
}

void mesh_heap::begin_frame(std::uint64_t frame)
{
    current_frame_ = frame;
    // Reclaim ranges whose deferred-free window has elapsed (every in-flight frame that might still
    // read them has finished), coalescing them back into the heaps for reuse.
    for (std::size_t i = 0; i < pending_free_.size();)
    {
        if (pending_free_[i].safe_frame <= frame)
        {
            vheap_.free(pending_free_[i].vheap, pending_free_[i].vcount);
            pending_free_[i] = pending_free_.back();
            pending_free_.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

bool mesh_heap::can_stream(::string::gpu::resource_id id, std::uint32_t /*to_detail*/)
{
    const DrawRange& r = ranges_[id];
    return vheap_.can_allocate(r.vertex_count);
}

std::uint64_t mesh_heap::stream(::string::gpu::resource_id id, std::uint32_t /*from_detail*/,
                                       std::uint32_t /*to_detail*/)
{
    const DrawRange& r = ranges_[id];
    if (r.index_count == 0)
    {
        return 0;
    }
    // The manager only promotes when can_stream() is true, so the allocation succeeds. Only the vertex
    // heap is uploaded now — the meshlet path pulls vertices via the meshlet-vertex remap; there is no
    // GPU index buffer (brief 04 M3). The CPU `indices_` were consumed by meshlet_builder at load.
    const std::optional<std::uint64_t> vo = vheap_.allocate(r.vertex_count);
    Alloc& a = allocs_[id];
    a.vheap = vo.value();
    a.resident = true;

    return transfer_.upload_buffer(&vertices_[r.vertex_offset], VkDeviceSize(r.vertex_count) * sizeof(Vertex),
                                   vertex_buffer_, a.vheap * sizeof(Vertex));
}

bool mesh_heap::is_complete(std::uint64_t ticket)
{
    return transfer_.is_complete(ticket);
}

void mesh_heap::on_resident(::string::gpu::resource_id id, std::uint32_t detail)
{
    if (detail < 1 || !set_cull_)
    {
        return;
    }
    const DrawRange& r = ranges_[id];
    const Alloc& a = allocs_[id];
    // Indices are global; vertex_offset rebases meshlet vertices from the source vertex numbering to
    // the draw's heap slot: heap_vertex = index_value + (a.vheap - r.vertex_offset). Stored as uint
    // bits of a signed int32 (the mesh shader reinterprets it with int()).
    const std::int32_t vertex_offset = static_cast<std::int32_t>(a.vheap) - static_cast<std::int32_t>(r.vertex_offset);
    set_cull_(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(vertex_offset), /*resident=*/1u);
    ++resident_draws_;
    streamed_bytes_ += cost(id, detail);
}

void mesh_heap::evict(::string::gpu::resource_id id, std::uint32_t /*from_detail*/, std::uint32_t to_detail)
{
    if (to_detail != 0)
    {
        return;
    }
    Alloc& a = allocs_[id];
    if (!a.resident)
    {
        return;
    }
    // Hide the draw now (resident 0), but defer freeing its heap ranges until every in-flight
    // frame that might still be drawing it has finished — then begin_frame() reclaims them.
    if (set_cull_) set_cull_(static_cast<std::uint32_t>(id), 0, /*resident=*/0u);
    const DrawRange& r = ranges_[id];
    pending_free_.push_back(PendingFree{ a.vheap, r.vertex_count,
                                         current_frame_ + frames_in_flight_ });
    a.resident = false;
    if (resident_draws_ > 0) --resident_draws_;
    ++evicted_draws_;
}

VkDeviceSize mesh_heap::cost(::string::gpu::resource_id id, std::uint32_t detail)
{
    if (detail == 0)
    {
        return 0;
    }
    const DrawRange& r = ranges_[id];
    return VkDeviceSize(r.vertex_count) * sizeof(Vertex);  // vertex heap only (no GPU index heap, M3)
}

}  // namespace string::assets
