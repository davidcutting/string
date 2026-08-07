// Ground-truth ambient occlusion + bent normals (half res, temporally reprojected). Brief 20 — the
// image rings, the bindless slots and the hand-rolled sampler are gone; the two targets are graph
// resources the application declares, and the chain is TWO declared passes so the raw -> denoise
// hand-over is a derived barrier rather than a vku::transition_image call. All three transitions this
// file used to emit are deleted; see the header for which declaration replaces which.
#include <algorithm>

#include <string/core/logger.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <string/render/gtao.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace String;

gtao_chain::gtao_chain(String::engine_context& ctx, GeometryScene* scene)
: device_(&ctx.device)
, descriptors_(&ctx.descriptor_table)
, scene_(scene)
, frames_in_flight_(scene->frames_in_flight_)
{
    // Published so GeometryPass's SceneData can read runs()/size(). The AO IMAGE is not published:
    // its consumer declares its own read and resolves the slot through pass_context.
    scene_->gtao = this;

    // No vkCreateSampler: the linear-clamp configuration the half-res upsample needs is declared on
    // the targets themselves (image_info::sampler), so bind() writes the right descriptor by itself.
    const VkDescriptorSetLayout layout = descriptors_->get_layout();
    const auto make_entry = [&](const char* entry) {
        return ctx.shader_registry.create(
            ctx.resources_path / "shaders" / "gtao.slang",
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
    program_ = make_entry("gtao_main");
    denoise_program_ = make_entry("gtao_denoise_main");
}

gtao_chain::~gtao_chain()
{
    if (scene_ != nullptr) scene_->gtao = nullptr;
    if (device_ == nullptr) return;
    const auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (prog == nullptr) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_->get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_->get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(program_);
    destroy_program(denoise_program_);
    program_ = denoise_program_ = nullptr;
}

uint16_t gtao_chain::prev_slot(uint16_t current_frame) const
{
    if (frames_in_flight_ == 0) return current_frame;
    return static_cast<uint16_t>((current_frame + frames_in_flight_ - 1) % frames_in_flight_);
}

void gtao_chain::tick(VkExtent2D extent, uint16_t current_frame)
{
    runs_this_frame_ = false;
    if (scene_ == nullptr) return;

    if (extent.width != 0 && extent.height != 0)
    {
        // The targets are declared viewport-scaled at 0.5, so this must be the same half-res rule the
        // graph allocates them by — it is what the dispatch grid and the push constants are sized to.
        const glm::uvec2 size((extent.width + 1) / 2, (extent.height + 1) / 2);
        if (size != size_)
        {
            size_ = size;
            STRING_LOG_INFO("[gtao] half-res targets {}x{}", size.x, size.y);
        }
    }

    // r.furnace validates the BRDF against an analytic environment, so AO must be off under it.
    const bool allowed = cv_gtao_enabled().get() && !scene_->furnace_;
    const uint16_t prev = prev_slot(current_frame);
    runs_this_frame_ = allowed && program_ != nullptr && denoise_program_ != nullptr
        && size_.x != 0 && size_.y != 0
        && prev < scene_->depth_history_.size() && scene_->depth_history_[prev].valid != 0;
}

// Author both halves of the chain.
//
//   gtao.raw       reads the previous slot's depth, writes `raw`   (GENERAL, first write -> discard)
//   gtao.denoise   reads `raw` (SHADER_READ_ONLY), writes `ao`
//
// The three deleted transitions map exactly onto these declarations: the raw target's first-frame
// discard onto gtao.raw's write, the raw -> denoise hand-over onto the write/read pair between the
// two passes, and the cross-frame write-after-read onto that same write against tracked state that
// now carries over the frame boundary.
void gtao_chain::declare(::string::frame_graph& fg, ::string::gpu::image depth_prev,
                         ::string::gpu::image raw, ::string::gpu::image ao)
{
    const auto enabled = [this] { return runs_this_frame_; };

    fg.pass("gtao.raw")
      .reads(depth_prev)
      .writes(raw)
      .toggle(enabled)
      .compute([this, depth_prev, raw](::string::pass_context& ctx) { record_raw(ctx, depth_prev, raw); });

    // The denoise is depth-aware (its bilateral weights sample the same reprojected depth), so it
    // reads depth_prev too — declared, not inherited from the other pass's push block.
    fg.pass("gtao.denoise")
      .reads(depth_prev)
      .reads(raw)
      .writes(ao)
      .toggle(enabled)
      .compute([this, depth_prev, raw, ao](::string::pass_context& ctx) {
          record_denoise(ctx, depth_prev, raw, ao);
      });
}

bool gtao_chain::base_push(::string::pass_context& ctx, GtaoPush& out) const
{
    if (scene_ == nullptr) return false;
    const uint16_t prev = prev_slot(static_cast<uint16_t>(ctx.frame_slot));
    if (prev >= scene_->depth_history_.size() || scene_->depth_history_[prev].valid == 0) return false;
    // The depth IMAGE comes from the graph (ctx.slot(depth_prev), in the caller); only the camera it
    // was rendered with is read here — CPU state captured alongside the slot, not a GPU resource.
    const DepthHistorySlot& hist = scene_->depth_history_[prev];

    const glm::mat4& proj = hist.proj;
    out = GtaoPush{};
    out.view = hist.view;
    out.inv_proj = glm::inverse(proj);
    out.dst_size = size_;
    out.depth_size = glm::uvec2(ctx.extent.width, ctx.extent.height);
    out.radius = std::max(cv_gtao_radius().get(), 0.01f);
    out.proj00 = std::abs(proj[0][0]);
    out.proj11 = std::abs(proj[1][1]);
    return true;
}

void gtao_chain::dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                          const GtaoPush& push) const
{
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const ::string::gpu::pipeline& p = prog->current();
    VkDescriptorSet set = descriptors_->get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1, &set, 0, nullptr);
    recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GtaoPush), &push);
    recorder.dispatch((size_.x + 7) / 8, (size_.y + 7) / 8, 1);
}

void gtao_chain::record_raw(::string::pass_context& ctx, ::string::gpu::image depth_prev,
                            ::string::gpu::image raw)
{
    GtaoPush push{};
    if (!base_push(ctx, push)) return;
    // Both slots come from this pass's own declarations: depth_prev was declared as a sampled read so
    // it resolves to a texture slot, raw as a storage-image write so it resolves to a storage slot.
    push.depth_slot = ctx.slot(depth_prev);
    push.dst_slot = ctx.slot(raw);
    dispatch(ctx, program_, push);
}

void gtao_chain::record_denoise(::string::pass_context& ctx, ::string::gpu::image depth_prev,
                                ::string::gpu::image raw, ::string::gpu::image ao)
{
    GtaoPush push{};
    if (!base_push(ctx, push)) return;
    push.depth_slot = ctx.slot(depth_prev);
    push.src_slot = ctx.slot(raw);
    push.dst_slot = ctx.slot(ao);
    dispatch(ctx, denoise_program_, push);
}

}  // namespace string::render
