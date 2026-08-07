#include <string/render/geometry/sky_component.hpp>

#include <utility>

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

namespace string::render
{
using namespace String;

sky_component::sky_component(engine_context& ctx, VkSampleCountFlagBits samples)
: device_(ctx.device)
{
    // Procedural: no descriptor bindings, only a push constant — reflection reports zero sets and the
    // push-constant range. Built via the hot-reload registry (recompiles + swaps on save).
    program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "sky_shader.slang",
        [samples](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
            ::string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({})
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            ::string::gpu::pipeline_builder builder(dev);
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
                .set_multisampling(samples)
                .enable_depth_stencil(false, false)   // background: declare depth format but don't test/write
                .enable_color_blending()
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        });
}

sky_component::~sky_component()
{
    if (!program_) return;
    const ::string::gpu::pipeline& p = program_->current();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
}

void sky_component::tick(const SkyParams& params)
{
    params_ = params;
}

void sky_component::declare(::string::frame_graph& fg, ::string::gpu::image color,
                            ::string::gpu::image depth, ::string::enable_fn enabled)
{
    fg.pass("sky")
      .color(color)
      .depth_read(depth)
      .toggle(std::move(enabled))
      .raster([this](::string::pass_context& ctx) { record(ctx); });
}

void sky_component::record(::string::pass_context& ctx)
{
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const SkyPush sky_push{
        .inv_view_proj = glm::inverse(params_.view_proj),
        .camera_pos = params_.camera_pos,
        .furnace = params_.furnace ? 1.0f : 0.0f,
        .sun_dir = params_.sun_dir,
        .sky_zenith = params_.sky_zenith,
        .sky_ground = params_.sky_ground,   // albedo; radiance derived in-shader
        .sun_color = params_.sun_color,
        .sun_intensity = params_.sun_intensity,
    };
    const ::string::gpu::pipeline& sky_p = program_->current();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, sky_p.pipeline);
    recorder.push_constants(sky_p.pipeline_layout,
                            sky_p.push_constants.stageFlags, 0, sizeof(SkyPush), &sky_push);
    recorder.draw(3, 1, 0, 0);
}

}  // namespace string::render
