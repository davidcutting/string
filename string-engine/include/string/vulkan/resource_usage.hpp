#pragma once

#include <cstdint>

#include <string/vulkan/resource.hpp>

#include <volk.h>

namespace String
{

// How a pass uses a resource. This is the single typed vocabulary shared by the executable
// passes (Pass::usages) and the render-graph planner (render_graph.hpp): a future graph uses
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
    SampledRead,    // sampled image (texture)
    StorageRead,    // storage buffer / image read
    StorageWrite,   // storage buffer / image write
    VertexRead,     // vertex buffer (attribute fetch)
    IndexRead,      // index buffer
    TransferRead,   // copy source
    TransferWrite,  // copy destination
    Present,        // ready for the presentation engine
};

constexpr bool is_write(Access access)
{
    switch (access)
    {
        case Access::ColorWrite:
        case Access::DepthWrite:
        case Access::StorageWrite:
        case Access::TransferWrite:
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
            return { VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        case Access::DepthWrite:
            // DEPTH_STENCIL_* (not the separate DEPTH_* layouts, which need the
            // separateDepthStencilLayouts feature we don't enable).
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        case Access::DepthRead:
            return { VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        case Access::SampledRead:
            return { VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        case Access::StorageRead:
            return { VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::StorageWrite:
            return { VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::VertexRead:
            return { VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::IndexRead:
            return { VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
        case Access::TransferRead:
            return { VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        case Access::TransferWrite:
            return { VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
        case Access::Present:
            return { 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    }
    return { 0, VK_IMAGE_LAYOUT_UNDEFINED };
}

// A single resource use declared by a pass: which resource, how it's accessed, and the
// pipeline stage(s) at which this pass touches it.
struct ResourceUsage
{
    ResourceID resource;
    Access access;
    VkPipelineStageFlags2 stage;
};

}  // namespace String
