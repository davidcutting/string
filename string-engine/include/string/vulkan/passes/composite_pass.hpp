#pragma once

#include <filesystem>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pipeline.hpp>
#include <string/vulkan/device.hpp>

#include <volk.h>

namespace String
{

// Fullscreen pass that samples one bindless texture (the offscreen HDR target) and writes
// it to the swapchain. It binds the global bindless descriptor set and pushes the source
// image's slot; the fragment shader indexes the sampler array by that slot.
class CompositePass final : public Pass
{
    Device& device_;
    Pipeline pipeline_;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    uint32_t source_slot_ = 0;

public:
    CompositePass(Device& device, const std::filesystem::path& resources_path,
                  VkDescriptorSetLayout global_layout, VkFormat color_format);
    virtual ~CompositePass() override;

    // The renderer supplies the global set and the slot the HDR target is bound at
    // (re-supplied on resize, when the target is re-created and re-bound).
    void set_source(VkDescriptorSet descriptor_set, uint32_t source_slot);

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual void record(CommandRecorder& recorder, uint16_t current_frame) override;
};

}
