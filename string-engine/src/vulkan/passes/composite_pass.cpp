#include <string/vulkan/passes/composite_pass.hpp>
#include <string/gpu/pipeline_builder.hpp>

namespace String
{

CompositePass::CompositePass(string::gpu::device& device, const std::filesystem::path& resources_path,
                             VkDescriptorSetLayout global_layout, VkFormat color_format)
: device_(device)
{
    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = 2 * sizeof(uint32_t),   // { source_slot, exposure }
    };

    pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
        .set_descriptor_set_layout({ global_layout })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    // Fullscreen triangle sampling the bindless HDR target into the swapchain: no depth,
    // no blending, target format is the swapchain's (set via set_color_format).
    pipeline_.pipeline = string::gpu::pipeline_builder(device_)
        .add_vertex_shader(resources_path / "shaders/composite.vert.spv")
        .add_fragment_shader(resources_path / "shaders/composite.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .disable_color_blending()
        .set_color_format(color_format)
        .build_graphics_pipeline(pipeline_.pipeline_layout);

    pipeline_.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
}

CompositePass::~CompositePass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);
}

void CompositePass::set_source(VkDescriptorSet descriptor_set, uint32_t source_slot)
{
    descriptor_set_ = descriptor_set;
    source_slot_ = source_slot;
}

void CompositePass::update(float delta_time, uint16_t current_frame)
{
    (void)delta_time;
    (void)current_frame;
}

void CompositePass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    (void)current_frame;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
    // { source_slot, exposure } — exposure scales the HDR before the tonemap curve (see composite.frag).
    struct { uint32_t source_slot; float exposure; } push{ source_slot_, 1.0f };
    vkCmdPushConstants(command_buffer, pipeline_.pipeline_layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}
