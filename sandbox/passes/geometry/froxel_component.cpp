#include "froxel_component.hpp"

#include "../lighting_data.hpp"

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/core/logger.hpp>

namespace sandbox
{

void FroxelComponent::init(String::engine_context& context, uint32_t frames_in_flight)
{
    resources_ = &context.resources;
    frames_in_flight_ = frames_in_flight;
    // Actual allocation is deferred to ensure_capacity() (called from FroxelPass::resize() once the
    // screen size is known — the deterministic point, before any per-frame update). The registry-owned
    // PerFrame handle stays invalid until then.

    // Froxel light-binning compute pipeline (Slang, hot-reloadable).
    program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "froxel_cull.slang",
        [](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::COMPUTE);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT)
                    builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
            }
            p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::COMPUTE;
            return p;
        });
}

void FroxelComponent::ensure_capacity(VkExtent2D screen)
{
    if (screen.width == 0 || screen.height == 0)
    {
        return;
    }
    tiles_x_ = (screen.width + kFroxelTileSize - 1) / kFroxelTileSize;
    tiles_y_ = (screen.height + kFroxelTileSize - 1) / kFroxelTileSize;
    count_ = tiles_x_ * tiles_y_ * kFroxelDepthSlices;
    if (count_ <= capacity_ && froxel_buffer_.valid())
    {
        return;   // fits the existing allocation
    }
    // Grow (or first allocation). The renderer waits the device idle on resize, so recreating these
    // device-addressed buffers here is safe. Stride = [count, idx...] = 1 + max per froxel.
    const VkDeviceSize stride = (1 + kMaxLightsPerFroxel) * sizeof(uint32_t);
    const VkDeviceSize size = stride * count_;
    const string::gpu::buffer_info info{
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    };
    if (froxel_buffer_.valid()) resources_->recreate_per_frame(froxel_buffer_, info);
    else                        froxel_buffer_ = resources_->create_per_frame(info, frames_in_flight_);
    capacity_ = count_;
    STRING_LOG_INFO("[froxel] grid {}x{}x{} = {} froxels ({} MB/frame)", tiles_x_,
                    tiles_y_, kFroxelDepthSlices, count_, size / (1024 * 1024));
}

void FroxelComponent::record(string::gpu::command_recorder& recorder, uint16_t frame, const FroxelParams& params)
{
    if (!active(frame)) return;
    const string::gpu::pipeline& fp = program_->current();
    const FroxelPush fpush{
        .view = params.view,
        .inv_proj = params.inv_proj,
        .screen = params.screen,
        .grid = glm::uvec2(tiles_x_, tiles_y_),
        .slices = kFroxelDepthSlices,
        .tile_size = kFroxelTileSize,
        .near_plane = params.near_plane,
        .far_plane = params.far_plane,
        .light_count = params.light_count,
        .max_per_froxel = kMaxLightsPerFroxel,
        ._pad0 = 0, ._pad1 = 0,
        .lights = params.lights,
        .froxels = resources_->address(froxel_buffer_, frame),
    };
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, fp.pipeline);
    recorder.push_constants(fp.pipeline_layout, fp.push_constants.stageFlags,
                            0, sizeof(FroxelPush), &fpush);
    recorder.dispatch((tiles_x_ + 3) / 4, (tiles_y_ + 3) / 4, (kFroxelDepthSlices + 3) / 4);
}

void FroxelComponent::destroy()
{
    // The froxel index ring is owned + freed by the ResourceRegistry now (brief 16 M2); nothing to
    // free here. (The compute pipeline is torn down by the owning FroxelPass destructor.)
}

VkDeviceAddress FroxelComponent::froxels_address(uint16_t frame) const
{
    if (!froxel_buffer_.valid()) return 0;
    return resources_->address(froxel_buffer_, frame);
}

}  // namespace sandbox
