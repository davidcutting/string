#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/vulkan_utils.hpp>

namespace String
{

void ResourceStateTracker::transition(VkCommandBuffer command_buffer, VkImage image,
    VkImageAspectFlags aspect, Access access, VkPipelineStageFlags2 stage, bool discard)
{
    ResourceState& current = images_[image];   // default: UNDEFINED, no prior access
    const AccessScope target = access_scope(access);
    const bool write = is_write(access);
    const bool layout_change = discard || target.layout != current.layout;

    if (!write && !layout_change)
    {
        // Pure read in the current layout. Free if the last write is already visible to this
        // (stage, access); otherwise make it visible (RAW) without a layout change.
        if ((current.visible_stages & stage) == stage
            && (current.visible_access & target.access) == target.access)
        {
            current.reader_stages |= stage;
            return;
        }
        vku::transition_image(command_buffer, {
            .image = image,
            .old_layout = current.layout,
            .new_layout = current.layout,
            .src_stage = current.last_write_stage,
            .src_access = current.last_write_access,
            .dst_stage = stage,
            .dst_access = target.access,
            .aspect = aspect,
        });
        current.reader_stages |= stage;
        current.visible_stages |= stage;
        current.visible_access |= target.access;
        return;
    }

    // Write and/or layout change: order against the last write (WAW/availability) AND every
    // reader since it (WAR — execution dependency; readers need no availability).
    VkPipelineStageFlags2 src_stage = current.last_write_stage | current.reader_stages;
    if (src_stage == 0) src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    vku::transition_image(command_buffer, {
        .image = image,
        .old_layout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : current.layout,
        .new_layout = target.layout,
        .src_stage = src_stage,
        .src_access = current.last_write_access,
        .dst_stage = stage,
        .dst_access = target.access,
        .aspect = aspect,
    });
    current.layout = target.layout;
    current.last_write_stage = stage;
    current.last_write_access = target.access;
    current.reader_stages = 0;
    current.visible_stages = stage;
    current.visible_access = target.access;
}

void ResourceStateTracker::buffer_access(string::gpu::resource_id resource, Access access,
                                         VkPipelineStageFlags2 stage)
{
    ResourceState& current = buffers_[resource];
    const AccessScope target = access_scope(access);

    if (!is_write(access))
    {
        if ((current.visible_stages & stage) == stage
            && (current.visible_access & target.access) == target.access)
        {
            current.reader_stages |= stage;
            return;
        }
        // RAW: make the last write visible to this read. A buffer never written under tracking
        // (static upload, host-written ring) has no hazard here — its producer's barrier
        // (transfer batch / host submission guarantee) already covers it.
        if (current.last_write_access != 0)
        {
            pending_src_stage_ |= current.last_write_stage;
            pending_src_access_ |= current.last_write_access;
            pending_dst_stage_ |= stage;
            pending_dst_access_ |= target.access;
        }
        current.reader_stages |= stage;
        current.visible_stages |= stage;
        current.visible_access |= target.access;
        return;
    }

    // Write: hazard only if something was tracked before (first-ever access needs no barrier).
    const bool prior_write = current.last_write_access != 0;
    const bool prior_read = current.reader_stages != 0;
    if (prior_write || prior_read)
    {
        pending_src_stage_ |= current.last_write_stage | current.reader_stages;
        pending_src_access_ |= current.last_write_access;
        pending_dst_stage_ |= stage;
        pending_dst_access_ |= target.access;
    }
    current.last_write_stage = stage;
    current.last_write_access = target.access;
    current.reader_stages = 0;
    current.visible_stages = stage;
    current.visible_access = target.access;
}

void ResourceStateTracker::flush_buffers(VkCommandBuffer command_buffer)
{
    if (pending_dst_stage_ == 0) return;
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = pending_src_stage_,
        .srcAccessMask = pending_src_access_,
        .dstStageMask = pending_dst_stage_,
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
    vkCmdPipelineBarrier2(command_buffer, &dependency);
    pending_src_stage_ = 0;
    pending_src_access_ = 0;
    pending_dst_stage_ = 0;
    pending_dst_access_ = 0;
}

void ResourceStateTracker::clear()
{
    images_.clear();
    buffers_.clear();
    pending_src_stage_ = 0;
    pending_src_access_ = 0;
    pending_dst_stage_ = 0;
    pending_dst_access_ = 0;
}

}  // namespace String
