#pragma once

#include <cstdint>
#include <vector>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pass_context.hpp>
#include <string/vulkan/pipeline.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/resource.hpp>
#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/descriptor_allocator.hpp>
#include <string/core/layout.hpp>

#include <volk.h>

namespace sandbox
{

// Static bindless UI pass: takes an already-laid-out set of nodes (core/layout.hpp), packs
// them into a persistent storage buffer of colored rounded/bordered rects, binds it into the
// bindless table, and instance-draws them as an overlay on the offscreen target. The layout
// *authoring* (which elements, their colors/shapes) is the application's job — this pass owns
// only the GPU packing and drawing. Dynamic (per-frame rebuilt) UI is a later, additive step.
class UIPass final : public String::Pass
{
    String::Device& device_;
    String::ResourceAllocator& allocator_;
    String::DescriptorTable& descriptor_table_;

    String::Pipeline pipeline_;

    String::ResourceID shape_buffer_;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    uint32_t shape_slot_ = 0;
    uint32_t shape_count_ = 0;

public:
    // `nodes` is the output of a layout_builder (copied so the pass owns it). The application
    // builds the layout; the pass turns it into GPU shapes.
    UIPass(String::PassContext& context, std::vector<string::layout_node> nodes);
    virtual ~UIPass() override;

    virtual void record(String::CommandRecorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox
