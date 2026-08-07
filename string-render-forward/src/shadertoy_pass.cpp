#include <string/render/shadertoy_pass.hpp>

#include <utility>

#include <string/core/logger.hpp>
#include <string/gpu/pipeline_builder.hpp>

namespace string::render
{
using namespace String;

shadertoy_pass::shadertoy_pass(engine_context& ctx, std::filesystem::path shader,
                               VkSampleCountFlagBits samples)
: device_(ctx.device)
, shader_(std::move(shader))
{
    auto builder = [samples](::string::gpu::device& dev,
                             const ::string::gpu::compiled_program& compiled) {
        ::string::gpu::pipeline p{};
        p.push_constants = compiled.layout.push_constant;
        p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({})
            .set_push_constant_ranges({ compiled.layout.push_constant })
            .build(dev);

        ::string::gpu::pipeline_builder pb(dev);
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
        p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
        return p;
    };

    // A user shader broken at startup must not be fatal — see the class comment. The registry
    // throws (correctly, for engine shaders); catch it here and come up empty, so the scene still
    // loads, the error overlay shows why, and the next save fixes it.
    try
    {
        program_ = ctx.shader_registry.create(shader_, builder);
    }
    catch (const std::exception& e)
    {
        STRING_LOG_WARN("[shadertoy] {} did not compile: {}", shader_.string(), e.what());
        STRING_LOG_WARN("[shadertoy] the scene is loaded and watching the file — fix and save");
        program_ = nullptr;
    }
}

shadertoy_pass::~shadertoy_pass()
{
    if (program_ == nullptr)
    {
        return;
    }
    const ::string::gpu::pipeline& p = program_->current();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
}

void shadertoy_pass::tick(float dt)
{
    time_ += dt;
    dt_ = dt;
    ++frame_;
}

void shadertoy_pass::declare(::string::frame_graph& fg, ::string::gpu::image color,
                             ::string::gpu::image depth)
{
    fg.pass("shadertoy")
      .color(color)
      .depth_read(depth)
      .raster([this](::string::pass_context& ctx) { record(ctx); });
}

void shadertoy_pass::record(::string::pass_context& ctx)
{
    ::string::gpu::command_recorder& recorder = ctx.rec;
    if (program_ == nullptr)
    {
        return;   // never compiled; the overlay is the user's feedback, not a crash
    }
    const ::string::gpu::pipeline& p = program_->current();

    const Push push{
        { static_cast<float>(ctx.extent.width), static_cast<float>(ctx.extent.height) },
        time_,
        dt_,
        { 0.0f, 0.0f, 0.0f, 0.0f },   // iMouse: wired to the input system in a later pass
        frame_,
        { 0.0f, 0.0f, 0.0f },
    };
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    recorder.push_constants(p.pipeline_layout, p.push_constants.stageFlags, 0, sizeof(Push), &push);
    recorder.draw(3, 1, 0, 0);
}

}  // namespace string::render
