#include <string/vulkan/passes/composite_pass.hpp>
#include <string/gpu/pipeline_builder.hpp>

namespace String
{

CompositePass::CompositePass(string::gpu::device& device, const std::filesystem::path& resources_path,
                             VkDescriptorSetLayout global_layout, VkFormat color_format,
                             string::gpu::shader_program_registry& registry)
: device_(device)
{
    // The pipeline is built from compiled Slang + reflection and owned by the shader_program, so a
    // save recompiles + swaps it. Set 0 stays the real bindless table layout (reflection can't
    // reproduce its update-after-bind/variable-count flags); reflection drives the push-constant
    // range (offset/size/stages) — the layout plumbing that actually varies per pass.
    program_ = registry.create(
        resources_path / "shaders" / "composite.slang",
        [global_layout, color_format](string::gpu::device& dev,
                                      const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            const VkPushConstantRange range = compiled.layout.has_push_constant
                ? compiled.layout.push_constant
                : VkPushConstantRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(uint32_t) };

            p.push_constants = range;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ global_layout })
                .set_push_constant_ranges({ range })
                .build(dev);

            string::gpu::pipeline_builder builder(dev);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                    builder.add_vertex_shader_spirv(stage.spirv, stage.entry_point);
                else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
            }
            // Fullscreen triangle sampling the bindless HDR target into the swapchain: no depth,
            // no blending, target format is the swapchain's.
            p.pipeline = builder
                .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .set_tessellation()
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .set_multisampling()
                .disable_color_blending()
                .set_color_format(color_format)
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });
}

CompositePass::~CompositePass()
{
    const string::gpu::pipeline& p = program_->current();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
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
    const string::gpu::pipeline& p = program_->current();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
    // { source_slot, exposure } — exposure scales the HDR before the tonemap curve (see composite.slang).
    struct { uint32_t source_slot; float exposure; } push{ source_slot_, 1.0f };
    vkCmdPushConstants(command_buffer, p.pipeline_layout,
        p.push_constants.stageFlags, 0, sizeof(push), &push);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}  // namespace String
