#pragma once

#include <cstdint>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace string
{

class compiled_frame;

// Brief 20 — the execute-time view handed to a pass's recording callback (the reference doc's
// TaskInterface, renamed). It bundles the frame's command_recorder with the current frame slot and
// viewport, and RESOLVES the pass's declared logical handles to this frame's physical backing.
//
// Resolve-at-execute is the structural fix for the address-passing crash class: a pass reads an
// address or a bindless slot through the context while it records, per this frame's backing, rather
// than latching one at some earlier point in an ordering nothing enforced.
//
// It is also where graceful degrade lands. slot()/id()/address() consult the pass's own declaration
// for the handle, so when a producer is toggled off the consumer transparently receives the
// resource's declared NEUTRAL fallback instead of dangling backing. The pass does not branch on it
// and does not know it happened — that is why toggling a pass off cannot produce a black screen.
//
//   engine_context   build-time services; passes are BUILT from it
//   pass_context     execute-time; resolves what the pass DECLARED, for this frame slot
struct pass_context
{
    gpu::command_recorder& rec;
    std::uint32_t frame_slot = 0;
    VkExtent2D extent{};

    // --- resolution ------------------------------------------------------------------------------
    // The physical backing of a declared handle, this frame.
    gpu::resource_id id(gpu::image h) const;
    gpu::resource_id id(gpu::buffer h) const;
    VkImage          vk_image(gpu::image_view v) const;
    VkImageView      view(gpu::image_view v) const;
    VkDeviceAddress  address(gpu::buffer h) const;
    void*            mapped(gpu::buffer h) const;

    // The bindless descriptor slot for a handle this pass declared. The descriptor TYPE is derived
    // from the declaration — a handle declared with sampled_read resolves to its texture slot, one
    // declared with a storage-image access resolves to its storage slot — so pass code never names a
    // descriptor type and never receives a slot from outside.
    std::uint32_t slot(gpu::image_view v) const;
    // The slot for a handle under a SPECIFIC declared access. A pass that both samples and
    // storage-writes one image in the same dispatch (a hysteresis read-modify-write) needs two
    // descriptor types for it, and the one-argument form can only return the first declared.
    std::uint32_t slot(gpu::image_view v, access how) const;
    std::uint32_t slot(gpu::image h) const { return slot(h.whole()); }
    std::uint32_t slot(gpu::image h, access how) const { return slot(h.whole(), how); }
    std::uint32_t slot(gpu::buffer h) const;

    // --- executor wiring (set by compiled_frame::execute; not for pass code) ----------------------
    const compiled_frame* frame = nullptr;
    std::uint32_t pass_index = 0;
};

}  // namespace string
