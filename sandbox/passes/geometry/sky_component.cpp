#include "sky_component.hpp"

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

namespace sandbox
{

void SkyComponent::init(String::PassContext& context)
{
    // Procedural: no descriptor bindings, only a push constant — reflection reports zero sets and the
    // push-constant range. Built via the hot-reload registry (recompiles + swaps on save).
    VkSampleCountFlagBits sky_samples = context.sample_count;
    program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "sky_shader.slang",
        [sky_samples](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({})
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                    builder.add_vertex_shader_spirv(stage.spirv, stage.entry_point);
                else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
            }
            p.pipeline = builder
                .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .set_tessellation()
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .set_multisampling(sky_samples)
                .enable_depth_stencil(false, false)   // background: declare depth format but don't test/write
                .enable_color_blending()
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });
}

void SkyComponent::record(VkCommandBuffer command_buffer, const SkyParams& params)
{
    const SkyPush sky_push{
        .inv_view_proj = glm::inverse(params.view_proj),
        .camera_pos = params.camera_pos,
        .furnace = params.furnace ? 1.0f : 0.0f,
        .sun_dir = params.sun_dir,
        .sky_zenith = params.sky_zenith,
        .sky_ground = params.sky_ground,   // albedo; radiance derived in-shader
        .sun_color = params.sun_color,
        .sun_intensity = params.sun_intensity,
    };
    const string::gpu::pipeline& sky_p = program_->current();
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_p.pipeline);
    vkCmdPushConstants(command_buffer, sky_p.pipeline_layout,
                       sky_p.push_constants.stageFlags, 0, sizeof(SkyPush), &sky_push);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}  // namespace sandbox
