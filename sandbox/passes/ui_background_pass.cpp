#include "ui_background_pass.hpp"

#include <utility>

#include <string/gpu/pipeline_builder.hpp>

namespace sandbox
{
using namespace String;

UIBackgroundPass::UIBackgroundPass(engine_context& context, std::shared_ptr<UiScene> scene,
                                   std::uint32_t anchor_count)
: device_(context.device)
, scene_(std::move(scene))
, driver_(anchor_count)
{
    usages = {
        { context.color_target, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };

    VkSampleCountFlagBits samples = context.sample_count;
    auto builder = [samples](string::gpu::device& dev,
                             const string::gpu::compiled_program& compiled) {
        string::gpu::pipeline p{};
        p.push_constants = compiled.layout.push_constant;
        p.pipeline_layout = string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({})
            .set_push_constant_ranges({ compiled.layout.push_constant })
            .build(dev);

        string::gpu::pipeline_builder pb(dev);
        for (const auto& stage : compiled.stages)
        {
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                pb.add_vertex_shader_spirv(stage.spirv, stage.entry_point);
            else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                pb.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
        }
        p.pipeline = pb
            .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .set_tessellation()
            .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .set_multisampling(samples)
            .enable_depth_stencil(false, false)
            .enable_color_blending()
            .build_graphics_pipeline(p.pipeline_layout);
        p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
        return p;
    };

    program_ = context.shader_registry.create(context.resources_path / "shaders" / "ui_background.slang",
                                               builder);
}

UIBackgroundPass::~UIBackgroundPass()
{
    const string::gpu::pipeline& p = program_->current();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
}

void UIBackgroundPass::update(float delta_time, uint16_t /*current_frame*/)
{
    time_ += delta_time;
    driver_.update(delta_time, screen_size.width, screen_size.height, *scene_);
}

void UIBackgroundPass::record(string::gpu::command_recorder& recorder, uint16_t /*current_frame*/)
{
    const string::gpu::pipeline& p = program_->current();

    const Push push{
        { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) }, time_, 0.0f };
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    recorder.push_constants(p.pipeline_layout, p.push_constants.stageFlags, 0, sizeof(Push), &push);
    recorder.draw(3, 1, 0, 0);
}

}  // namespace sandbox
