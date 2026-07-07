#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/vulkan_utils.hpp>

namespace String
{

void ResourceStateTracker::transition(VkCommandBuffer command_buffer, VkImage image,
    VkImageAspectFlags aspect, Access access, VkPipelineStageFlags2 stage, bool discard)
{
    ImageState& current = states_[image];   // default {UNDEFINED, TOP_OF_PIPE, 0} for a new image
    const AccessScope target = access_scope(access);

    vku::transition_image(command_buffer, {
        .image = image,
        .old_layout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : current.layout,
        .new_layout = target.layout,
        .src_stage = current.stage,
        .src_access = current.access,
        .dst_stage = stage,
        .dst_access = target.access,
        .aspect = aspect,
    });

    current = { target.layout, stage, target.access };
}

void ResourceStateTracker::clear()
{
    states_.clear();
}

}  // namespace String
