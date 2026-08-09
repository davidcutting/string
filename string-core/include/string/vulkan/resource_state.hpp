#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace string
{

// The current sync2 state of one tracked subresource: the last write's scope, the reader stages
// accumulated since that write, and (images) the layout. `visible_*` records where the last write has
// already been made visible, so a redundant read-after-read barrier is skipped.
struct sync_state
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 last_write_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2 last_write_access = 0;
    VkPipelineStageFlags2 reader_stages = 0;
    VkPipelineStageFlags2 visible_stages = 0;
    VkAccessFlags2 visible_access = 0;

    bool operator==(const sync_state& o) const
    {
        return layout == o.layout && last_write_stage == o.last_write_stage
            && last_write_access == o.last_write_access && reader_stages == o.reader_stages
            && visible_stages == o.visible_stages && visible_access == o.visible_access;
    }
};

// The slice of an image a transition applies to. Tracking is PER SUBRESOURCE, which is what makes a
// mip chain (each level written from the one above) and a cubemap (each face written separately)
// expressible: the graph derives the chain instead of a `level_count` parameter transitioning every
// mip at once under an assumption nothing checks.
struct subresource
{
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t base_mip = 0;
    std::uint32_t mip_count = 1;
    std::uint32_t base_layer = 0;
    std::uint32_t layer_count = 1;
};

// Derives sync2 barriers from tracked state plus a target access — callers say "I need this
// subresource for a color_write at this stage" and never hand-write a barrier. Hazard-complete:
//   - a WRITE barriers against the last write AND every reader since it (WAW + WAR), even when the
//     layout does not change;
//   - a READ barriers only if the last write is not yet visible to its (stage, access) — repeat
//     readers are free;
//   - BUFFERS are tracked by resource_id and their hazards merge into ONE global memory barrier per
//     flush point, with minimal united scopes.
//
// This class IS the derived-barrier emitter. It is not an exception to "no hand-rolled barriers" —
// it is the implementation of that rule.
class resource_state_tracker
{
public:
    // Register an image's shape so subresource tracking has somewhere to live. Idempotent; called by
    // the executor when it first touches an image.
    void track(VkImage image, VkImageAspectFlags aspect, std::uint32_t mip_levels,
               std::uint32_t array_layers);

    // Barrier `image`'s `sub` into the layout + access that `how` requires at `stage`, then update
    // the tracked state. `discard` sources from UNDEFINED, for a subresource whose contents are
    // fully overwritten. Emits nothing when the request is already satisfied. Subresources whose
    // tracked state matches are coalesced into a single barrier.
    void transition(gpu::command_recorder& rec, VkImage image, const subresource& sub, access how,
                    VkPipelineStageFlags2 stage, bool discard = false);

    // Same, for a write the FRAMEWORK performs rather than a pass declaring it — currently only the
    // multisample resolve at vkCmdEndRendering, whose scope no declared access describes: a DEPTH
    // resolve lands in the depth-attachment layout, but the validation layer (and the spec's
    // render-pass model) attribute the write itself to COLOR_ATTACHMENT_WRITE at
    // COLOR_ATTACHMENT_OUTPUT. A barrier that names only the depth stages therefore neither permits
    // the resolve nor covers it for the next reader. This is not new declaration vocabulary — no
    // pass can name it; it is the framework describing its own write to the tracker, which is
    // exactly what deleting `Access::DepthResolve` from the DECLARED surface presumed.
    void transition_scope(gpu::command_recorder& rec, VkImage image, const subresource& sub,
                          const access_scope& target, VkPipelineStageFlags2 stage,
                          bool write, bool discard = false);

    // Note a buffer access, accumulating any required global memory-barrier scopes into the pending
    // merge. flush_buffers() emits it before the consuming commands are recorded.
    void buffer_access(gpu::resource_id resource, access how, VkPipelineStageFlags2 stage);
    void flush_buffers(gpu::command_recorder& rec);

    // Forget all tracked state (frame boundary with recreated backing, or a resize).
    void clear();

    // Overwrite an image's tracked layout without emitting a barrier, for a layout changed outside
    // the graph (the debug capture, which drains the device and transitions directly). This is a
    // NOTIFICATION, not a back door for deriving sync — the tracker stays the single authority.
    void set_layout(VkImage image, VkImageLayout layout);

    // While set, emitted barriers are recorded on a COMPUTE-ONLY queue's command buffer, so any
    // graphics-only source/destination stage (fragment, attachment output, task/mesh…) is widened to
    // ALL_COMMANDS — that stage cannot be named on this queue, and the cross-QUEUE half of the edge
    // is carried by the lane timeline semaphores, not by this barrier. Tracked state keeps the REAL
    // stages; only the emission is legalized.
    void set_queue_scope(bool compute_only) { compute_only_queue_ = compute_only; }

private:
    struct image_track
    {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        std::uint32_t mips = 1;
        std::uint32_t layers = 1;
        std::vector<sync_state> cells;   // mips * layers, mip-major
        sync_state& at(std::uint32_t mip, std::uint32_t layer) { return cells[mip * layers + layer]; }
    };

    VkPipelineStageFlags2 legalize(VkPipelineStageFlags2 stages) const;
    bool compute_only_queue_ = false;

    std::unordered_map<VkImage, image_track> images_;
    std::unordered_map<gpu::resource_id, sync_state> buffers_;

    VkPipelineStageFlags2 pending_src_stage_ = 0;
    VkAccessFlags2 pending_src_access_ = 0;
    VkPipelineStageFlags2 pending_dst_stage_ = 0;
    VkAccessFlags2 pending_dst_access_ = 0;
};

}  // namespace string
