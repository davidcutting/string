#pragma once

#include <cstdint>

#include <string/gpu/resource.hpp>
#include <string/gpu/resource_registry.hpp>

#include <volk.h>

namespace String
{

// How a pass uses a resource. This is the single typed vocabulary shared by the executable
// passes (Pass::usages) and the render-graph planner (graph_plan.hpp): a future graph uses
// it to order passes and — via access_scope() — to derive sync2 barriers between them.
//
// The Access category fixes the *access mask* and *image layout* a use implies (the intrinsic,
// error-prone part). The *pipeline stage* is carried per-usage on ResourceUsage, because the
// same access can happen at different stages (e.g. a storage buffer read in the vertex vs the
// fragment stage), which a category alone can't capture.
enum class Access : std::uint8_t
{
    ColorWrite,     // color attachment write
    DepthWrite,     // depth attachment test + write
    DepthRead,      // depth attachment read-only test
    // Brief 11 P2 (B1): a MARKER usage naming the single-sample image a raster group MIN-resolves its
    // MSAA depth into at EndRendering (reverse-Z: min = farthest = conservative HiZ occluder). It
    // replaces the Pass::depth_resolve_target() virtual hook: the group loop scans a group's usages
    // for this access to find the resolve target, instead of asking the breaking pass. NOT a graph
    // write (stays out of written_resources_) — the resolve target is pass-managed/untracked state;
    // this only identifies it. The non-UNDEFINED layout keeps it off the buffer-usage path.
    DepthResolve,
    SampledRead,    // sampled image (texture)
    StorageRead,    // storage buffer / image read
    StorageWrite,   // storage buffer / image write
    VertexRead,     // vertex buffer (attribute fetch)
    IndexRead,      // index buffer
    IndirectRead,   // indirect draw/dispatch parameter read (brief 04e M2)
    TransferRead,   // copy source
    TransferWrite,  // copy destination
    Present,        // ready for the presentation engine
    // Brief 09: storage-IMAGE accesses (post-processing compute reading/writing render targets in
    // GENERAL layout). Distinct from StorageRead/StorageWrite, whose UNDEFINED layout marks them as
    // buffer usages throughout the renderer. StorageImageWrite's scope also covers SAMPLED reads —
    // post compute passes commonly sample a target they then write back (bloom apply).
    StorageImageRead,
    StorageImageWrite,
};

constexpr bool is_write(Access access)
{
    switch (access)
    {
        case Access::ColorWrite:
        case Access::DepthWrite:
        case Access::StorageWrite:
        case Access::TransferWrite:
        case Access::StorageImageWrite:
            return true;
        default:
            return false;
    }
}

// The access mask + required image layout implied by an Access. `layout` is
// VK_IMAGE_LAYOUT_UNDEFINED where it doesn't apply (buffers). Combine with a usage's stage to
// build a VkImageMemoryBarrier2 / VkMemoryBarrier2 (Stage 2 consumes this).
struct AccessScope
{
    VkAccessFlags2 access;
    VkImageLayout layout;
};

constexpr AccessScope access_scope(Access access)
{
    switch (access)
    {
        case Access::ColorWrite:
            // WRITE|READ: color attachment use also READS — LOAD_OP_LOAD, blending, and the
            // MSAA resolve's source read all happen in the attachment-output stage. The 04d
            // ghosting bug class is exactly a missed store->load/resolve dependency; the write
            // scope must cover both directions (brief 04e M2).
            return { VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        case Access::DepthWrite:
            // DEPTH_STENCIL_* (not the separate DEPTH_* layouts, which need the
            // separateDepthStencilLayouts feature we don't enable).
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        case Access::DepthRead:
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        case Access::DepthResolve:
            // Marker only (see the enum comment). The scope is never consumed for a barrier — the
            // resolve target is untracked pass-managed state — but the layout must be non-UNDEFINED so
            // the renderer's is_buffer_usage() check doesn't misroute this image id onto the buffer path.
            return { VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        case Access::SampledRead:
            return { VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        case Access::StorageRead:
            return { VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::StorageWrite:
            // WRITE|READ: storage writes are commonly read-modify-write (atomics — the
            // visibility bitfield, stats counters), so a write use must order against BOTH
            // directions of the previous access (brief 04e M2).
            return { VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::VertexRead:
            return { VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::IndexRead:
            return { VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::IndirectRead:
            return { VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::TransferRead:
            return { VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        case Access::TransferWrite:
            return { VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
        case Access::Present:
            return { 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        case Access::StorageImageRead:
            return { VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL };
        case Access::StorageImageWrite:
            // WRITE|READ|SAMPLED: post compute both samples and storage-writes the target in one
            // record hook (histogram + bloom read the scene color, bloom-apply writes it back).
            return { VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                         | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL };
    }
    return { 0, VK_IMAGE_LAYOUT_UNDEFINED };
}

// A single resource use declared by a pass: how it's accessed, at which pipeline stage(s), and WHICH
// resource — declared as a LOGICAL registry handle (brief 16: the graph is the single resolution
// authority; the executor resolves the per-frame physical via resolve(), NOT the pass). A raw
// `resource` id is still accepted for the not-yet-virtualized cases (renderer sentinels
// COLOR/DEPTH/SWAPCHAIN_TARGET, persistent buffers, imported ids); exactly one of {buf, img, resource}
// identifies the resource. This replaces the old pattern where each pass pre-resolved
// resources->physical(handle, slot) into `resource` — resolution now lives in ONE place.
struct ResourceUsage
{
    string::gpu::resource_id resource = 0;   // raw id (sentinels/persistent) when no handle is set
    Access access = Access::SampledRead;
    VkPipelineStageFlags2 stage = 0;
    string::gpu::buffer buf{};               // logical PerFrame buffer handle (resolved per frame)
    string::gpu::image  img{};               // logical PerFrame image handle (resolved per frame)

    // The single EXECUTION resolution point: a logical handle -> this frame's physical id; else raw.
    string::gpu::resource_id resolve(const string::gpu::ResourceRegistry& reg, std::uint32_t slot) const
    {
        if (buf.valid()) return reg.physical(buf, slot);
        if (img.valid()) return reg.physical(img, slot);
        return resource;
    }

    // The stable LOGICAL identity the planner keys on for ordering (producer->consumer edges +
    // lifetimes). A handle's identity is the handle itself (stable across frames — the physical
    // rotates but the logical resource is one thing), mapped into reserved id bands that never collide
    // with allocator ids (small, from 0), the renderer sentinels (top of the space), or graph
    // transients (1<<48). No physical resolution — that's resolve()'s job, at execute.
    static constexpr string::gpu::resource_id BUFFER_HANDLE_BASE = string::gpu::resource_id{ 1 } << 50;
    static constexpr string::gpu::resource_id IMAGE_HANDLE_BASE  = string::gpu::resource_id{ 1 } << 51;
    string::gpu::resource_id key() const
    {
        if (buf.valid()) return BUFFER_HANDLE_BASE | buf.index;
        if (img.valid()) return IMAGE_HANDLE_BASE | img.index;
        return resource;
    }
};

}  // namespace String
