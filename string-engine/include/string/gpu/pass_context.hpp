#pragma once

#include <cstdint>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource_registry.hpp>

#include <volk.h>

namespace string::gpu
{

// Brief 16 — Layer 3 (execution context). The execute-time view handed to each pass callback by the
// executor (== Daxa's TaskInterface, renamed). It bundles the frame's command_recorder with the
// resource registry + the CURRENT frame slot, and resolves a pass's logical handles to THIS frame's
// physical backing on demand. That resolve-at-execute is the structural fix for the address-passing
// crash class: a pass reads froxel/scene/etc. addresses through ctx at record time, per this frame's
// physical, instead of baking one at update() time (which was order-fragile and device-lost the host
// during the brief-11 finalization).
//
//   engine_context (build-time services, `String::engine_context`, string/vulkan/engine_context.hpp)
//        -> passes are BUILT from it, declare their logical handles
//   pass_context (this) — handed per-invocation at EXECUTE time, resolves handles for `frame_slot`
//        -> holds command_recorder& rec
//
// Lightweight: built inline by the executor per pass invocation (all references, no ownership).
//
// NOTE: distinct from `String::engine_context` (the BUILD-time services struct) — the M7 rename
// (PassContext -> engine_context + pass_context.hpp -> engine_context.hpp) freed this name.
struct pass_context
{
    command_recorder& rec;
    ResourceRegistry& resources;
    std::uint32_t frame_slot;

    // Resolve a logical handle to this frame's physical backing. PerFrame handles use frame_slot;
    // Imported/Persistent ignore it. These are the verbs a pass body calls instead of touching a
    // resource_id or reaching into the allocator.
    VkDeviceAddress address(buffer h) const { return resources.address(h, frame_slot); }
    resource_id     id     (buffer h) const { return resources.physical(h, frame_slot); }
    void*           mapped (buffer h) const { return resources.mapped(h, frame_slot); }

    resource_id     id  (image h) const { return resources.physical(h); }
    VkImageView     view(image h) const { return resources.view(h); }
};

}  // namespace string::gpu
