#pragma once

#include <cstdint>
#include <filesystem>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/resource.hpp>
#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/descriptor_allocator.hpp>

#include <volk.h>

namespace String
{

// Static bindless UI pass: builds one layout (via core/layout.hpp) at construction, packs
// its nodes into a persistent storage buffer of colored rects, binds it into the bindless
// table, and instance-draws them as an overlay on the offscreen target. Dynamic (per-frame
// rebuilt) UI and rounded corners/borders are later, additive steps.
class UIPass final : public Pass
{
    Device& device_;
    ResourceAllocator& allocator_;
    DescriptorTable& descriptor_table_;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    ResourceID shape_buffer_;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    uint32_t shape_slot_ = 0;
    uint32_t shape_count_ = 0;

public:
    UIPass(Device& device, ResourceAllocator& allocator, DescriptorTable& descriptor_table,
           const std::filesystem::path& resources_path, const uint16_t& frames_in_flight);
    virtual ~UIPass() override;

    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) override;
};

}
