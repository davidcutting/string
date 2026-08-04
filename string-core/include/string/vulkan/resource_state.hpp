#pragma once

#include <unordered_map>

#include <string/gpu/resource.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace String
{

// The current sync2 state of a tracked resource: the last write's scope, the accumulated reader
// stages since that write, and (for images) the layout. `visible_*` is where the last write has
// already been made visible, so redundant read-after-read barriers are skipped.
struct ResourceState
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // images only
    VkPipelineStageFlags2 last_write_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2 last_write_access = 0;
    VkPipelineStageFlags2 reader_stages = 0;
    VkPipelineStageFlags2 visible_stages = 0;
    VkAccessFlags2 visible_access = 0;
};

// Tracks each resource's state and derives sync2 barriers from tracked state plus a target
// Access (via access_scope) — callers say "I need this resource for a ColorWrite at this stage"
// rather than hand-writing barriers. Brief 04e M2: this is now HAZARD-COMPLETE, the execution
// substrate the render graph drives from pass-declared usages:
//   - a WRITE emits a barrier against the last write AND all readers since it (WAW + WAR),
//     even when the layout doesn't change (the 04d msaa ghosting class);
//   - a READ emits a barrier only if the last write isn't yet visible to its (stage, access)
//     — repeat readers are free;
//   - BUFFERS are tracked by resource_id; their hazards accumulate into ONE merged global
//     memory barrier per flush (minimal united scopes).
class ResourceStateTracker
{
    std::unordered_map<VkImage, ResourceState> images_;
    std::unordered_map<string::gpu::resource_id, ResourceState> buffers_;

    // Pending merged buffer barrier scopes (flushed as a single VkMemoryBarrier2).
    VkPipelineStageFlags2 pending_src_stage_ = 0;
    VkAccessFlags2 pending_src_access_ = 0;
    VkPipelineStageFlags2 pending_dst_stage_ = 0;
    VkAccessFlags2 pending_dst_access_ = 0;

public:
    // Record a barrier moving `image` to the layout + access that `access` requires at `stage`,
    // then update the tracked state. `discard` drops the current contents (source layout
    // UNDEFINED) — for targets that are fully cleared/overwritten each use. Emits nothing when
    // the request is already satisfied (visible read, no hazard). `level_count` = mip levels to
    // transition (default 1 = single-mip; pass the image's full mip count for a mip chain like the
    // HiZ pyramid, which is uniform across mips at every pass boundary — intra-pass per-mip
    // divergence during its own reduce is handled by that pass's local barriers, not the tracker).
    void transition(VkCommandBuffer command_buffer, VkImage image, VkImageAspectFlags aspect,
                    Access access, VkPipelineStageFlags2 stage, bool discard = false,
                    uint32_t level_count = 1);

    // Brief 11 step 2b: SEED a resource's state without emitting a barrier — for a write the tracker
    // cannot observe through transition()/buffer_access(), namely a render-pass attachment RESOLVE
    // (the two-phase MSAA depth -> hz.depth min-resolve executes at the group's EndRendering in the
    // COLOR_ATTACHMENT_OUTPUT stage). After seeding, a later transition() derives the correct
    // wait-on-resolve. Overwrites any prior tracked state for `image`.
    void seed(VkImage image, VkImageLayout layout, VkPipelineStageFlags2 write_stage,
              VkAccessFlags2 write_access);
    // Is this image currently tracked (seeded or previously transitioned)? Lets the renderer skip
    // static uploaded inputs (never tracked) while still transitioning seeded resolve targets.
    bool is_tracked(VkImage image) const { return images_.count(image) != 0; }

    // Note a buffer access; accumulates any required global memory-barrier scopes into the
    // pending merge. Call flush_buffers() before the consuming commands are recorded.
    void buffer_access(string::gpu::resource_id resource, Access access,
                       VkPipelineStageFlags2 stage);
    // Emit the merged pending buffer barrier (if any) into the command buffer.
    void flush_buffers(VkCommandBuffer command_buffer);

    // Forget all tracked state (e.g. after the swapchain + attachments are recreated on resize).
    void clear();
};

}  // namespace String
