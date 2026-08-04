#include <algorithm>
#include <limits>

#include <string/render/geometry_streamer.hpp>

namespace string::render
{
using namespace String;

GeometryStreamer::GeometryStreamer(::string::gpu::resource_allocator& allocator, TransferBatch& transfer,
                                   ::string::gpu::resource_id vertex_buffer,
                                   std::vector<Vertex> vertices, std::vector<std::uint32_t> indices,
                                   std::uint64_t vertex_capacity,
                                   std::uint32_t frames_in_flight)
: allocator_(allocator)
, transfer_(transfer)
, vertex_buffer_(vertex_buffer)
, vertices_(std::move(vertices))
, indices_(std::move(indices))
, vheap_(vertex_capacity)
, frames_in_flight_(frames_in_flight)
{
    (void)allocator_;  // held for symmetry with other providers
}

GeometryStreamer::GeometryStreamer(::string::gpu::resource_allocator& allocator, TransferBatch& transfer,
                                   ::string::gpu::resource_id vertex_buffer,
                                   std::vector<Vertex> vertices,
                                   std::uint64_t vertex_capacity,
                                   std::uint32_t frames_in_flight)
: allocator_(allocator)
, transfer_(transfer)
, vertex_buffer_(vertex_buffer)
, vertices_(std::move(vertices))
, vheap_(vertex_capacity)
, frames_in_flight_(frames_in_flight)
{
    (void)allocator_;  // held for symmetry with other providers
}

void GeometryStreamer::set_windows(const std::vector<Window>& windows)
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

void GeometryStreamer::set_draws(const std::vector<GltfDraw>& draws)
{
    ranges_.resize(draws.size());
    allocs_.assign(draws.size(), Alloc{});
    for (std::size_t i = 0; i < draws.size(); ++i)
    {
        const GltfDraw& d = draws[i];
        DrawRange r;
        r.index_offset = d.index_offset;
        r.index_count = d.index_count;

        std::uint32_t vmin = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t vmax = 0;
        for (std::uint32_t k = 0; k < d.index_count; ++k)
        {
            const std::uint32_t v = indices_[d.index_offset + k];
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }
        if (d.index_count == 0) { vmin = 0; vmax = 0; }
        r.vertex_offset = vmin;
        r.vertex_count = (d.index_count == 0) ? 0 : (vmax - vmin + 1);
        ranges_[i] = r;
    }
}

void GeometryStreamer::begin_frame(std::uint64_t frame)
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

bool GeometryStreamer::can_stream(::string::gpu::resource_id id, std::uint32_t /*to_detail*/)
{
    const DrawRange& r = ranges_[id];
    return vheap_.can_allocate(r.vertex_count);
}

std::uint64_t GeometryStreamer::stream(::string::gpu::resource_id id, std::uint32_t /*from_detail*/,
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

bool GeometryStreamer::is_complete(std::uint64_t ticket)
{
    return transfer_.is_complete(ticket);
}

void GeometryStreamer::on_resident(::string::gpu::resource_id id, std::uint32_t detail)
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

void GeometryStreamer::evict(::string::gpu::resource_id id, std::uint32_t /*from_detail*/, std::uint32_t to_detail)
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

VkDeviceSize GeometryStreamer::cost(::string::gpu::resource_id id, std::uint32_t detail)
{
    if (detail == 0)
    {
        return 0;
    }
    const DrawRange& r = ranges_[id];
    return VkDeviceSize(r.vertex_count) * sizeof(Vertex);  // vertex heap only (no GPU index heap, M3)
}

}  // namespace string::render
