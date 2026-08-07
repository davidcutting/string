// The cascaded shadow-map technique: owns the depth-only meshlet program and the per-cascade work
// lists. Brief 20 — the cascade images, their sampler and their bindless slots are gone from here:
// they are graph resources the application declares, and this file's job is to say what each cascade
// draw TOUCHES and then draw it. It contained zero hand-rolled barriers before and still does.
#include <string>

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

#include <string/render/shadow_maps.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace String;

shadow_maps::shadow_maps(String::engine_context& ctx, GeometryScene* scene)
: device_(&ctx.device)
, allocator_(&ctx.allocator)
, descriptors_(&ctx.descriptor_table)
, scene_(scene)
, frames_in_flight_(scene->frames_in_flight_)
{
    // Published so the meshlet culler can reach the per-cascade work lists it FILLS but does not own,
    // and so SceneData can read the cascade count. The cascade IMAGES are no longer published here —
    // they are the application's graph handles and every consumer declares its own read of them.
    scene_->shadow = this;

    // Depth-only meshlet shadow pipeline (no cull; depth bias handles acne, off-frustum casters kept).
    const VkDescriptorSetLayout layout = descriptors_->get_layout();
    program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "meshlet_shadow.slang",
        [layout](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
            ::string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            ::string::gpu::pipeline_builder builder(dev);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_TASK_BIT_EXT)
                    builder.add_task_shader_spirv(stage.spirv, stage.entry_point);
                else if (stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT)
                    builder.add_mesh_shader_spirv(stage.spirv, stage.entry_point);
                else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);  // brief 04: cutout clip()
            }
            p.pipeline = builder
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_multisampling()
                .enable_depth_stencil()
                .depth_only()
                .build_mesh_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // Per-cascade work lists, reserved from the frame scratch arena. Same region size as the camera
    // lists (the meshlet build published the layout), and reserved HERE rather than alongside them —
    // FrameScratch::reserve is a construction-time API. The regions are per-frame transients rebuilt
    // by the cull/expand computes every frame, so their offsets carry no state.
    lists_.resize(frames_in_flight_);
    if (scene_->scratch_ != nullptr && scene_->wl_layout_.size > 0)
    {
        std::array<Worklist, kMaxCascades> shadow{};
        for (uint32_t c = 0; c < kMaxCascades; ++c)
            shadow[c] = Worklist{ 0, scene_->scratch_->reserve(scene_->wl_layout_.size) };
        for (uint32_t f = 0; f < frames_in_flight_; ++f) lists_[f] = shadow;
    }
}

shadow_maps::~shadow_maps()
{
    if (scene_ != nullptr) scene_->shadow = nullptr;
    if (program_ != nullptr)
    {
        const ::string::gpu::pipeline& p = program_->current();
        vkDestroyPipeline(device_->get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_->get_device(), p.pipeline_layout, nullptr);
        program_ = nullptr;
    }
}

void shadow_maps::tick()
{
    if (scene_ == nullptr) return;

    // Bind this run's work lists to the scratch slot buffers once the renderer has materialized the
    // arena (post-construction, pre-first-frame) — mirrors GeometryPass's own camera-list binding.
    if (!lists_bound_ && scene_->scratch_ != nullptr && scene_->scratch_->materialized())
    {
        for (uint32_t f = 0; f < frames_in_flight_ && f < lists_.size(); ++f)
        {
            const ::string::gpu::resource_id buf = scene_->scratch_->buffer(f);
            for (Worklist& wl : lists_[f]) wl.buffer = buf;
        }
        lists_bound_ = true;
    }
}

bool shadow_maps::ready() const
{
    return program_ != nullptr && scene_ != nullptr && lists_bound_
        && scene_->draw_count_ != 0 && scene_->draw_info_mapped_ != nullptr;
}

// Author onto the graph. One pass per cascade, because one pass per cascade is what it IS: each
// cascade is an independent depth-only render into its own image from its own work list. The
// declaration says only that — the depth attachment (so the framework opens the render pass and
// derives the reverse-Z clear from this being the image's first write) and the work list it consumes.
// Every layout, every barrier and the write-after-read edge against the previous frame's samples
// derive from that.
void shadow_maps::declare(::string::frame_graph& fg, std::span<const ::string::gpu::image> cascades,
                          ::string::gpu::buffer worklists)
{
    for (uint32_t c = 0; c < cascades.size() && c < kMaxCascades; ++c)
    {
        fg.pass("shadow.cascade" + std::to_string(c))
          .depth(cascades[c])
          // The two stages the work list is consumed at are genuinely different and the graph cannot
          // recover them from the pass kind: the indirect draw/count words are fetched at
          // DRAW_INDIRECT, the compacted records[] are read by the task shader. GeometryPass declares
          // the matching compute-stage write of the same buffer, which is what makes the
          // cull -> cascade edge causal rather than incidental.
          .reads(worklists, access::indirect_read)
          .reads(worklists, access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          // r.pass.shadow, plus "is there anything to draw". A skipped cascade is not a black screen:
          // its consumers read the declared neutral (1.0, unshadowed) fallback.
          .toggle([this] { return cv_pass_shadow().get() && ready(); })
          .raster([this, c, worklists](::string::pass_context& ctx) { record_cascade(ctx, c, worklists); });
    }
}

void shadow_maps::record_cascade(::string::pass_context& ctx, uint32_t cascade,
                                 ::string::gpu::buffer worklists)
{
    if (cascade >= cascade_count()) return;
    GeometryScene& s = *scene_;
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const WorklistLayout& wl = s.wl_layout_;

    // No vkCmdBeginRendering here: the pass declared .depth(cascade), so the framework opens the
    // render-pass instance with the load/store and the reverse-Z clear derived from this being the
    // image's first write of the frame.
    const ::string::gpu::pipeline& msh = program_->current();
    VkDescriptorSet set = descriptors_->get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, msh.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS, msh.pipeline_layout, 0, 1, &set, 0, nullptr);

    // Meshlet shadow path: task/mesh depth-only, frustum-culled per cascade (no HiZ, no cone cull —
    // backfacing meshlets still cast). Brief 03b: reuses the CAMERA work list's {draw_index,
    // camera-selected LOD} records; the per-cascade meshlet frustum cull happens in the shadow task
    // shader against this light_view_proj.
    MeshletShadowPush mspush{};
    mspush.light_view_proj = s.cascade_view_proj_[cascade];
    mspush.vertices = allocator_->get_buffer(s.vertex_buffer_).device_address;
    mspush.meshlets = allocator_->get_buffer(s.meshlet_buffer_).device_address;
    mspush.mverts = allocator_->get_buffer(s.meshlet_vertices_).device_address;
    mspush.mtris = allocator_->get_buffer(s.meshlet_triangles_).device_address;
    mspush.draws = allocator_->get_buffer(s.draw_info_buffer_).device_address;
    // Brief 04c: each cascade draws its OWN worklist (resident-only, draw-culled vs THIS cascade's
    // light sphere; camera-selected LOD via the shared draw_lod). One indirect draw.
    const VkDeviceSize offset = lists_[ctx.frame_slot][cascade].offset;
    mspush.records = ctx.address(worklists) + offset + wl.records_off;
    const VkBuffer sh_buf = allocator_->get_buffer(ctx.id(worklists)).buffer;
    recorder.push_constants(msh.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletShadowPush), &mspush);
    recorder.draw_mesh_tasks_indirect_count(sh_buf, offset + wl.commands_off,
                                            sh_buf, offset + wl.count_off,
                                            s.cull_max_draws_, sizeof(uint32_t) * 3);
}

}  // namespace string::render
