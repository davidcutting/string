#include <cstdlib>
#include <string/core/logger.hpp>
#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <algorithm>

namespace string
{

void resource_state_tracker::track(VkImage image, VkImageAspectFlags aspect,
                                   std::uint32_t mip_levels, std::uint32_t array_layers)
{
    image_track& t = images_[image];
    const std::uint32_t want_mips = std::max(mip_levels, 1u);
    const std::uint32_t want_layers = std::max(array_layers, 1u);
    if (!t.cells.empty() && want_mips <= t.mips && want_layers <= t.layers) return;

    // GROW rather than return early. An image first touched through a SLICE sized the grid to that
    // slice; a later whole-image use then clamped itself to the smaller grid and silently transitioned
    // only part of the image while believing it had done all of it. That is how a whole-cube sampled
    // read ended up re-emitting barriers with stale state for mips it had never actually covered.
    // Existing cells keep their state; new ones start UNDEFINED, which is the truth about them.
    const std::uint32_t new_mips = std::max(want_mips, t.mips);
    const std::uint32_t new_layers = std::max(want_layers, t.layers);
    std::vector<sync_state> grown(static_cast<std::size_t>(new_mips) * new_layers);
    // Copy cell-by-cell with the OLD stride on the read side and the NEW stride on the write side —
    // the two differ whenever the layer count grew, and only copy what actually exists.
    const std::uint32_t copy_mips = std::min(t.mips, new_mips);
    const std::uint32_t copy_layers = std::min(t.layers, new_layers);
    for (std::uint32_t m = 0; m < copy_mips && !t.cells.empty(); ++m)
        for (std::uint32_t l = 0; l < copy_layers; ++l)
            grown[static_cast<std::size_t>(m) * new_layers + l] =
                t.cells[static_cast<std::size_t>(m) * t.layers + l];

    t.aspect = aspect;
    t.mips = new_mips;
    t.layers = new_layers;
    t.cells = std::move(grown);
}

void resource_state_tracker::transition(VkCommandBuffer cmd, VkImage image, const subresource& sub,
                                        access how, VkPipelineStageFlags2 stage, bool discard)
{
    transition_scope(cmd, image, sub, scope_of(how), stage, is_write(how), discard);
}

void resource_state_tracker::transition_scope(VkCommandBuffer cmd, VkImage image,
                                              const subresource& sub, const access_scope& target,
                                              VkPipelineStageFlags2 stage, bool write, bool discard)
{
    image_track& t = images_[image];
    if (t.cells.empty())
    {
        // First touch without an explicit track(): assume the declared shape.
        t.aspect = sub.aspect;
        t.mips = std::max(sub.base_mip + sub.mip_count, 1u);
        t.layers = std::max(sub.base_layer + sub.layer_count, 1u);
        t.cells.assign(static_cast<std::size_t>(t.mips) * t.layers, sync_state{});
    }

    // Clamping to the tracked grid is only safe because track() grows it to fit — otherwise a
    // whole-image use silently covers a subset while recording that it covered everything.
    const std::uint32_t mip_end = std::min(sub.base_mip + sub.mip_count, t.mips);
    const std::uint32_t layer_end = std::min(sub.base_layer + sub.layer_count, t.layers);

    // Walk the requested cells and coalesce maximal runs whose tracked state is identical, so the
    // common whole-image case emits exactly one barrier and a genuinely divergent mip chain emits
    // only as many as it actually needs.
    for (std::uint32_t layer = sub.base_layer; layer < layer_end; ++layer)
    {
        std::uint32_t mip = sub.base_mip;
        while (mip < mip_end)
        {
            const sync_state before = t.at(mip, layer);
            std::uint32_t run = 1;
            while (mip + run < mip_end && t.at(mip + run, layer) == before) ++run;

            // Resolved once: this sits in the per-subresource loop of every transition, so a getenv
            // + strtoull per cell per frame was the cost of a lever that cannot change at runtime.
            static const std::uintptr_t traced_image = [] {
                const char* e = std::getenv("STRING_TRACE_IMG");
                return e != nullptr ? std::strtoull(e, nullptr, 0) : 0ull;
            }();
            if (traced_image != 0 && reinterpret_cast<std::uintptr_t>(image) == traced_image)
                STRING_LOG_INFO("[trk] mip={} run<= layer={} old={} want={} write={} discard={}", mip, layer, int(before.layout), int(target.layout), write, discard);
            const bool layout_change = discard || target.layout != before.layout;
            sync_state after = before;

            if (!write && !layout_change)
            {
                if ((before.visible_stages & stage) == stage
                    && (before.visible_access & target.mask) == target.mask)
                {
                    after.reader_stages |= stage;
                }
                else
                {
                    string::vku::transition_image(cmd, {
                        .image = image,
                        .old_layout = before.layout,
                        .new_layout = before.layout,
                        .src_stage = legalize(before.last_write_stage),
                        .src_access = before.last_write_access,
                        .dst_stage = legalize(stage),
                        .dst_access = target.mask,
                        .aspect = t.aspect,
                        .base_mip = mip,
                        .level_count = run,
                        .base_layer = layer,
                        .layer_count = 1,
                    });
                    after.reader_stages |= stage;
                    after.visible_stages |= stage;
                    after.visible_access |= target.mask;
                }
            }
            else
            {
                // Write and/or layout change: order against the last write (WAW / availability) AND
                // every reader since it (WAR — an execution dependency; readers need no availability).
                VkPipelineStageFlags2 src_stage = before.last_write_stage | before.reader_stages;
                if (src_stage == 0) src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                string::vku::transition_image(cmd, {
                    .image = image,
                    .old_layout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : before.layout,
                    .new_layout = target.layout,
                    .src_stage = legalize(src_stage),
                    .src_access = before.last_write_access,
                    .dst_stage = legalize(stage),
                    .dst_access = target.mask,
                    .aspect = t.aspect,
                    .base_mip = mip,
                    .level_count = run,
                    .base_layer = layer,
                    .layer_count = 1,
                });
                after.layout = target.layout;
                after.last_write_stage = stage;
                after.last_write_access = target.mask;
                after.reader_stages = 0;
                after.visible_stages = stage;
                after.visible_access = target.mask;
            }
            for (std::uint32_t i = 0; i < run; ++i) t.at(mip + i, layer) = after;
            mip += run;
        }
    }
}

void resource_state_tracker::buffer_access(gpu::resource_id resource, access how,
                                           VkPipelineStageFlags2 stage)
{
    sync_state& current = buffers_[resource];
    const access_scope target = scope_of(how);

    if (!is_write(how))
    {
        if ((current.visible_stages & stage) == stage
            && (current.visible_access & target.mask) == target.mask)
        {
            current.reader_stages |= stage;
            return;
        }
        // RAW: make the last write visible to this read. A buffer never written under tracking
        // (a static upload, a host-written ring) has no hazard here — its producer's own
        // guarantee already covers it.
        if (current.last_write_access != 0)
        {
            pending_src_stage_ |= current.last_write_stage;
            pending_src_access_ |= current.last_write_access;
            pending_dst_stage_ |= stage;
            pending_dst_access_ |= target.mask;
        }
        current.reader_stages |= stage;
        current.visible_stages |= stage;
        current.visible_access |= target.mask;
        return;
    }

    const bool prior_write = current.last_write_access != 0;
    const bool prior_read = current.reader_stages != 0;
    if (prior_write || prior_read)
    {
        pending_src_stage_ |= current.last_write_stage | current.reader_stages;
        pending_src_access_ |= current.last_write_access;
        pending_dst_stage_ |= stage;
        pending_dst_access_ |= target.mask;
    }
    current.last_write_stage = stage;
    current.last_write_access = target.mask;
    current.reader_stages = 0;
    current.visible_stages = stage;
    current.visible_access = target.mask;
}

// Widen stages a compute-only queue cannot name to ALL_COMMANDS. The cross-queue half of such an
// edge is ordered by the lane timeline semaphores; this barrier only has to cover same-queue work,
// and ALL_COMMANDS on the compute queue is exactly (and legally) that.
VkPipelineStageFlags2 resource_state_tracker::legalize(VkPipelineStageFlags2 stages) const
{
    if (!compute_only_queue_ || stages == 0) return stages;
    constexpr VkPipelineStageFlags2 kComputeLegal =
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
        | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
        | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT
        | VK_PIPELINE_STAGE_2_HOST_BIT;
    if ((stages & ~kComputeLegal) == 0) return stages;
    return (stages & kComputeLegal) | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

void resource_state_tracker::flush_buffers(VkCommandBuffer cmd)
{
    if (pending_dst_stage_ == 0) return;
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = legalize(pending_src_stage_),
        .srcAccessMask = pending_src_access_,
        .dstStageMask = legalize(pending_dst_stage_),
        .dstAccessMask = pending_dst_access_,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    pending_src_stage_ = 0;
    pending_src_access_ = 0;
    pending_dst_stage_ = 0;
    pending_dst_access_ = 0;
}

void resource_state_tracker::set_layout(VkImage image, VkImageLayout layout)
{
    const auto it = images_.find(image);
    if (it == images_.end()) return;
    for (sync_state& s : it->second.cells) s.layout = layout;
}

void resource_state_tracker::clear()
{
    images_.clear();
    buffers_.clear();
    pending_src_stage_ = 0;
    pending_src_access_ = 0;
    pending_dst_stage_ = 0;
    pending_dst_access_ = 0;
}

}  // namespace string
