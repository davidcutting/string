#pragma once

#include <cstdint>

#include <string/gpu/resource.hpp>

#include <volk.h>

namespace string
{

// How a pass uses a resource. This is the single typed vocabulary the graph orders passes by and —
// via access_scope() — derives its sync2 barriers from.
//
// The `access` category fixes the *access mask* and *image layout* a use implies (the intrinsic,
// error-prone part). The *pipeline stage* is carried alongside on the declaration, because the same
// access can happen at different stages (a storage buffer read in the vertex vs the fragment stage),
// which a category alone can't capture.
enum class access : std::uint8_t
{
    color_write,     // color attachment write
    depth_write,     // depth attachment test + write
    depth_read,      // depth attachment read-only test
    sampled_read,    // sampled image (texture)
    storage_read,    // storage buffer read
    storage_write,   // storage buffer write
    vertex_read,     // vertex buffer (attribute fetch)
    index_read,      // index buffer
    indirect_read,   // indirect draw/dispatch parameter read
    transfer_read,   // copy source
    transfer_write,  // copy destination
    present,         // ready for the presentation engine
    // Storage-IMAGE accesses (compute reading/writing render targets in GENERAL layout). Distinct
    // from storage_read/storage_write, whose UNDEFINED layout marks them as buffer usages.
    storage_image_read,
    storage_image_write,
};

constexpr bool is_write(access a)
{
    switch (a)
    {
        case access::color_write:
        case access::depth_write:
        case access::storage_write:
        case access::transfer_write:
        case access::storage_image_write:
            return true;
        default:
            return false;
    }
}


// The access mask + required image layout implied by an access. `layout` is
// VK_IMAGE_LAYOUT_UNDEFINED where it doesn't apply (buffers). Combined with a declaration's stage to
// build a VkImageMemoryBarrier2 / VkMemoryBarrier2.
struct access_scope
{
    VkAccessFlags2 mask;
    VkImageLayout layout;
};

constexpr access_scope scope_of(access a)
{
    switch (a)
    {
        case access::color_write:
            // WRITE|READ: color attachment use also READS — LOAD_OP_LOAD, blending, and the MSAA
            // resolve's source read all happen in the attachment-output stage. A missed
            // store->load/resolve dependency is a ghosting bug, so the write scope covers both.
            return { VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        case access::depth_write:
            // DEPTH_STENCIL_* (not the separate DEPTH_* layouts, which need the
            // separateDepthStencilLayouts feature we don't enable).
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        case access::depth_read:
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        case access::sampled_read:
            return { VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        case access::storage_read:
            return { VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case access::storage_write:
            // WRITE|READ: storage writes are commonly read-modify-write (atomics — the visibility
            // bitfield, stats counters), so a write must order against BOTH directions.
            return { VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED };
        case access::vertex_read:
            return { VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case access::index_read:
            return { VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case access::indirect_read:
            return { VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case access::transfer_read:
            return { VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        case access::transfer_write:
            return { VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
        case access::present:
            return { 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        case access::storage_image_read:
            return { VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL };
        case access::storage_image_write:
            // WRITE|READ|SAMPLED: compute commonly samples a target it then storage-writes.
            return { VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                         | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL };
    }
    return { 0, VK_IMAGE_LAYOUT_UNDEFINED };
}

// A single declared resource use: WHICH resource (a logical image slice or a logical buffer), how it
// is accessed, and at which pipeline stage(s). Exactly one of {img, buf} is set. There is no raw
// resource_id here and no resolve()/key() multiplicity — the logical handle IS the identity the
// graph orders by, and the graph is the single authority that resolves it to physical at execute.
struct resource_use
{
    gpu::image_view img{};
    gpu::buffer buf{};
    access how = access::sampled_read;
    VkPipelineStageFlags2 stage = 0;

    bool is_image() const { return img.valid(); }
};

}  // namespace string
