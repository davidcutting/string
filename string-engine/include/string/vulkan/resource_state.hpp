#pragma once

#include <unordered_map>

#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace String
{

// The current sync2 state of an image: its layout and the scope of the last access, so the
// next transition can use them as its source.
struct ImageState
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2 access = 0;
};

// Tracks each image's current state and derives sync2 barriers from that tracked state plus a
// target Access (via access_scope) — so callers say "I need this image for a ColorWrite" rather
// than hand-writing old/new layouts and stage/access masks. This is the substrate Stage 3 (the
// render graph) will drive from pass usages; for now the renderer drives it for the frame's
// fixed targets.
class ResourceStateTracker
{
    std::unordered_map<VkImage, ImageState> states_;

public:
    // Record a barrier moving `image` to the layout + access that `access` requires at `stage`,
    // then update the tracked state. `discard` drops the current contents (source layout
    // UNDEFINED) — for targets that are fully cleared/overwritten each use.
    void transition(VkCommandBuffer command_buffer, VkImage image, VkImageAspectFlags aspect,
                    Access access, VkPipelineStageFlags2 stage, bool discard = false);

    // Forget all tracked state (e.g. after the swapchain + attachments are recreated on resize).
    void clear();
};

}  // namespace String
