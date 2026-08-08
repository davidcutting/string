#include <string/render/geometry/froxel_component.hpp>

#include <string/render/lighting_data.hpp>

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

namespace string::render
{
using namespace string;

froxel_component::froxel_component(engine_context& ctx)
: device_(ctx.device)
{
    // Froxel light-binning compute pipeline (Slang, hot-reloadable). Push-constant only: the light
    // and froxel buffers are reached by device address, so there is no descriptor set to bind.
    program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "froxel_cull.slang",
        [](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
            ::string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::COMPUTE);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT)
                    builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
            }
            p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::COMPUTE;
            return p;
        });
}

froxel_component::~froxel_component()
{
    if (!program_) return;
    const ::string::gpu::pipeline& p = program_->current();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
}

uint32_t froxel_component::tiles_x(VkExtent2D screen)
{
    return (screen.width + kFroxelTileSize - 1) / kFroxelTileSize;
}

uint32_t froxel_component::tiles_y(VkExtent2D screen)
{
    return (screen.height + kFroxelTileSize - 1) / kFroxelTileSize;
}

uint32_t froxel_component::froxel_count(VkExtent2D screen)
{
    return tiles_x(screen) * tiles_y(screen) * kFroxelDepthSlices;
}

// One [count, light indices...] record per froxel, and one froxel COLUMN (kFroxelDepthSlices deep)
// per screen tile. The app declares the buffer as `tile_pixels` + this, which is the same fact
// without a callback in the middle.
VkDeviceSize froxel_component::bytes_per_tile()
{
    const VkDeviceSize stride = (1 + kMaxLightsPerFroxel) * sizeof(uint32_t);
    return stride * kFroxelDepthSlices;
}

void froxel_component::tick(const FroxelParams& params)
{
    params_ = params;
}

void froxel_component::declare(::string::frame_graph& fg, ::string::gpu::buffer froxels,
                               ::string::gpu::buffer lights)
{
    fg.pass("froxel.cull")
      .reads(lights)
      .writes(froxels)
      .async()
      .toggle([this] { return has_work(); })
      .compute([this, froxels, lights](::string::pass_context& ctx) { record(ctx, froxels, lights); });
}

void froxel_component::record(::string::pass_context& ctx, ::string::gpu::buffer froxels,
                              ::string::gpu::buffer lights)
{
    const uint32_t tx = tiles_x(ctx.extent);
    const uint32_t ty = tiles_y(ctx.extent);
    if (tx == 0 || ty == 0) return;

    ::string::gpu::command_recorder& recorder = ctx.rec;
    const ::string::gpu::pipeline& fp = program_->current();
    const FroxelPush fpush{
        .view = params_.view,
        .inv_proj = params_.inv_proj,
        .screen = glm::uvec2(ctx.extent.width, ctx.extent.height),
        .grid = glm::uvec2(tx, ty),
        .slices = kFroxelDepthSlices,
        .tile_size = kFroxelTileSize,
        .near_plane = params_.near_plane,
        .far_plane = params_.far_plane,
        .light_count = params_.light_count,
        .max_per_froxel = kMaxLightsPerFroxel,
        ._pad0 = 0, ._pad1 = 0,
        .lights = params_.light_count > 0 ? ctx.address(lights) : 0,
        .froxels = ctx.address(froxels),
    };
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, fp.pipeline);
    recorder.push_constants(fp.pipeline_layout, fp.push_constants.stageFlags,
                            0, sizeof(FroxelPush), &fpush);
    recorder.dispatch((tx + 3) / 4, (ty + 3) / 4, (kFroxelDepthSlices + 3) / 4);
}

}  // namespace string::render
