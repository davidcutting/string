#include <string/render/geometry/ibl_component.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <glm/gtc/constants.hpp>

namespace string::render
{
using namespace string;

ibl_component::ibl_component(engine_context& ctx)
: device_(ctx.device)
, descriptor_set_(ctx.descriptor_table.get_set())
{
    // The five compute pipelines, one per ibl.slang entry point (hot-reload registry). No images, no
    // samplers, no per-mip views and no bindless slots are created here: the cubemaps, the DFG LUT
    // and the SH buffer are graph resources, and every slot the pushes carry is resolved from the
    // pass's own declaration while it records.
    VkDescriptorSetLayout layout = ctx.descriptor_table.get_layout();
    const auto make_ibl_entry = [&](const char* entry) {
        return ctx.shader_registry.create(
            ctx.resources_path / "shaders" / "ibl.slang",
            [layout, entry](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
                ::string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && stage.entry_point == entry)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = ::string::gpu::pipeline_type::COMPUTE;
                return p;
            });
    };
    env_capture_program_ = make_ibl_entry("capture_main");
    env_mip_program_ = make_ibl_entry("mip_main");
    env_prefilter_program_ = make_ibl_entry("prefilter_main");
    sh_project_program_ = make_ibl_entry("sh_project_main");
    dfg_program_ = make_ibl_entry("dfg_main");

    STRING_LOG_INFO("[ibl] env {}px cube x{} mips (capture) / x{} mips (prefiltered ladder), "
                    "DFG {}px, L2 SH", kEnvSize, kEnvCaptureMips, kEnvPrefilterMips, kDfgSize);
}

ibl_component::~ibl_component()
{
    const auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(env_capture_program_);
    destroy_program(env_mip_program_);
    destroy_program(env_prefilter_program_);
    destroy_program(sh_project_program_);
    destroy_program(dfg_program_);
}

void ibl_component::tick(const IblLighting& light, bool force_every_frame)
{
    light_ = light;
    constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg)
    const float align = glm::dot(glm::normalize(light.sun_dir), ibl_captured_sun_dir_);
    if (!ibl_primed_ || force_every_frame || light.furnace != ibl_captured_furnace_
        || align < kSunDeltaCos)
        ibl_update_pending_ = true;

    // One decision per frame, read by every pass's conditional. The bookkeeping that used to sit at
    // the end of record_update() belongs here with it: the chain is a CPU decision about whether to
    // record, and a pass body that mutated the decision would gate the rest of itself off.
    chain_this_frame_ = (ibl_update_pending_ || !dfg_baked_)
                     && env_capture_program_ != nullptr && env_capture_.valid();
    if (chain_this_frame_)
    {
        ibl_captured_sun_dir_ = glm::normalize(light.sun_dir);
        ibl_captured_furnace_ = light.furnace;
        ibl_primed_ = true;
        ibl_update_pending_ = false;
        ++ibl_update_count_;
    }
}

// The capture -> mip chain -> SH + prefilter ladder, as declared passes. Every ordering fact the nine
// hand-rolled barriers used to assert is a declaration here:
//   * capture mip m reads mip m-1 and writes mip m, so the chain derives from the slices themselves
//   * the SH projection names the ONE mip it reads (kShSourceMip), not the whole cube
//   * the prefilter mips write disjoint slices of the ladder and read the capture cube whole, so they
//     stay independent of each other — which is what the "mips independent" comment meant
//   * first use discards (no UNDEFINED->GENERAL prologue), the cross-frame WAR against last frame's
//     shading reads derives from tracked state, and the read edges into the lit fragments derive from
//     the consumers' own declarations
void ibl_component::declare(::string::frame_graph& fg,
                            ::string::gpu::image env_capture, ::string::gpu::image env_prefiltered,
                            ::string::gpu::image dfg_lut, ::string::gpu::buffer sh)
{
    env_capture_ = env_capture;
    env_prefiltered_ = env_prefiltered;
    dfg_lut_ = dfg_lut;
    sh_buffer_ = sh;

    const auto chain = [this] { return chain_this_frame_; };

    // Split-sum BRDF LUT: one-time, independent of the sky.
    fg.pass("ibl.dfg")
      .writes(dfg_lut_)
      .toggle([this] { return !dfg_baked_; })
      .compute([this](::string::pass_context& ctx) { record_dfg(ctx); });

    // 1) Sky -> capture mip 0 (all six faces).
    fg.pass("ibl.capture")
      .writes(env_capture_.mip(0))
      .toggle(chain)
      .compute([this](::string::pass_context& ctx) { record_capture(ctx); });

    // 2) Capture average chain (PDF-mip source + SH source), one pass per mip.
    for (uint32_t m = 1; m < kEnvCaptureMips; ++m)
    {
        fg.pass("ibl.capture.mip" + std::to_string(m))
          .reads(env_capture_.mip(m - 1), ::string::access::storage_image_read)
          .writes(env_capture_.mip(m))
          .toggle(chain)
          .compute([this, m](::string::pass_context& ctx) { record_capture_mip(ctx, m); });
    }

    // 3) L2 SH projection from exactly one capture mip.
    fg.pass("ibl.sh_project")
      .reads(env_capture_.mip(kShSourceMip), ::string::access::storage_image_read)
      .writes(sh_buffer_)
      .toggle(chain)
      .compute([this](::string::pass_context& ctx) { record_sh_project(ctx); });

    // 4) GGX prefilter ladder: each mip samples the whole capture cube (computed LOD) and writes its
    //    own slice of the ladder — six disjoint writes, no derived serialisation between them.
    for (uint32_t m = 0; m < kEnvPrefilterMips; ++m)
    {
        fg.pass("ibl.prefilter." + std::to_string(m))
          .reads(env_capture_)
          .writes(env_prefiltered_.mip(m))
          .toggle(chain)
          .compute([this, m](::string::pass_context& ctx) { record_prefilter(ctx, m); });
    }
}

IblPush ibl_component::base_push() const
{
    IblPush push{};
    push.sun_dir = glm::vec4(glm::normalize(light_.sun_dir), light_.furnace ? 1.0f : 0.0f);
    push.sky_zenith = glm::vec4(light_.sky_zenith, 0.0f);
    push.sky_ground = glm::vec4(light_.sky_ground, 0.0f);
    push.sun_color = glm::vec4(light_.sun_color, light_.sun_intensity);   // w: klx (ground-band lighting)
    return push;
}

void ibl_component::dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                             const IblPush& push, uint32_t gx, uint32_t gy, uint32_t gz) const
{
    const ::string::gpu::pipeline& p = prog->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    ctx.rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                 0, 1, &descriptor_set_, 0, nullptr);
    ctx.rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(IblPush), &push);
    ctx.rec.dispatch(gx, gy, gz);
}

void ibl_component::record_dfg(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.dst_slot = ctx.slot(dfg_lut_);
    push.dst_size = kDfgSize;
    push.sample_count = kDfgSamples;
    dispatch(ctx, dfg_program_, push, (kDfgSize + 7) / 8, (kDfgSize + 7) / 8, 1);
    dfg_baked_ = true;
}

void ibl_component::record_capture(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.dst_slot = ctx.slot(env_capture_.mip(0));
    push.dst_size = kEnvSize;
    dispatch(ctx, env_capture_program_, push, kEnvSize / 8, kEnvSize / 8, 6);
}

void ibl_component::record_capture_mip(::string::pass_context& ctx, uint32_t mip)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_.mip(mip - 1));
    push.dst_slot = ctx.slot(env_capture_.mip(mip));
    push.src_size = kEnvSize >> (mip - 1);
    push.dst_size = kEnvSize >> mip;
    dispatch(ctx, env_mip_program_, push, (push.dst_size + 7) / 8, (push.dst_size + 7) / 8, 6);
}

void ibl_component::record_sh_project(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_.mip(kShSourceMip));
    push.src_size = kEnvSize >> kShSourceMip;
    push.sh = ctx.address(sh_buffer_);
    dispatch(ctx, sh_project_program_, push, 1, 1, 1);
}

void ibl_component::record_prefilter(::string::pass_context& ctx, uint32_t mip)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_);            // SamplerCube: the whole capture chain
    push.src_size = kEnvSize;
    push.dst_slot = ctx.slot(env_prefiltered_.mip(mip));
    push.dst_size = kEnvSize >> mip;
    push.roughness = float(mip) / float(kEnvPrefilterMips - 1);
    push.sample_count = kPrefilterSamples;
    push.mip_count = kEnvCaptureMips;
    dispatch(ctx, env_prefilter_program_, push, (push.dst_size + 7) / 8, (push.dst_size + 7) / 8, 6);
}


}  // namespace string::render
