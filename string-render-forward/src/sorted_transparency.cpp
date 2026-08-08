// Sorted (CPU back-to-front) transparency: owns the blend pipeline and the CPU list build. Brief 20 —
// the per-frame list buffers are gone from here; the application declares them as a per-frame graph
// resource and this fills whichever one the frame slot resolves to. The sort and the fill are
// unchanged. It contained zero hand-rolled barriers before and still does.
#include <algorithm>
#include <utility>
#include <vector>

#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

#include <string/render/sorted_transparency.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace string;

// Host-visible list block in the SAME compacted per-draw shape the task shader consumes — commands[]
// (12 B) @0, records[] (8 B), then the count word. One function, called by the application when it
// declares the buffer and by this object when it fills it.
sorted_transparency::list_layout sorted_transparency::layout_for(uint32_t max_draws)
{
    const auto align16 = [](VkDeviceSize v) { return (v + 15) & ~VkDeviceSize(15); };
    list_layout l{};
    l.commands_off = 0;
    l.records_off  = align16(VkDeviceSize(sizeof(uint32_t) * 3) * max_draws);
    l.count_off    = align16(l.records_off + VkDeviceSize(sizeof(uint32_t) * 2) * max_draws);
    l.bytes        = l.count_off + 16;
    return l;
}

sorted_transparency::sorted_transparency(string::engine_context& ctx, GeometryScene* scene, VkSampleCountFlagBits samples)
: device_(&ctx.device)
, allocator_(&ctx.allocator)
, descriptors_(&ctx.descriptor_table)
, scene_(scene)
{
    samples_ = samples;
    // Blend, no depth write, two-sided (transparent surfaces show their back faces).
    const VkDescriptorSetLayout layout = descriptors_->get_layout();
    // brief 20: the sample count is declared by the app (see samples_), not supplied by the renderer.
    program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "meshlet_mesh.slang",
        [layout, samples](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
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
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
            }
            p.pipeline = builder
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_multisampling(samples)
                .enable_depth_stencil(/*depth_test*/ true, /*depth_write*/ false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                .enable_color_blending()
                .build_mesh_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        });
}

sorted_transparency::~sorted_transparency()
{
    if (device_ == nullptr) return;
    if (program_ != nullptr)
    {
        const ::string::gpu::pipeline& p = program_->current();
        vkDestroyPipeline(device_->get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_->get_device(), p.pipeline_layout, nullptr);
        program_ = nullptr;
    }
    blend_draws_.clear();
    ready_ = false;
}

void sorted_transparency::ensure()
{
    if (ready_ || scene_ == nullptr) return;
    if (scene_->draw_count_ == 0 || scene_->draw_info_mapped_ == nullptr
        || scene_->cull_max_draws_ == 0)
        return;
    ready_ = true;

    // The blend-flagged draws, collected once from the DrawInfo table the meshlet build filled.
    blend_draws_.clear();
    for (uint32_t i = 0; i < scene_->base_draw_count_; ++i)
        if (scene_->draw_info_mapped_[i].flags & kDrawFlagBlend) blend_draws_.push_back(i);

    layout_ = layout_for(scene_->cull_max_draws_);
}

uint32_t sorted_transparency::build_list(void* mapped)
{
    GeometryScene& s = *scene_;
    if (blend_draws_.empty() || mapped == nullptr) return 0;
    const glm::vec3 eye = s.camera_.position();

    // Sort a scratch copy back-to-front (farthest first) by squared distance eye->draw center.
    std::vector<std::pair<float, uint32_t>> order;
    order.reserve(blend_draws_.size());
    for (uint32_t di : blend_draws_)
    {
        if (di >= s.active_draw_count_) continue;
        const GpuDrawInfo& info = s.draw_info_mapped_[di];
        if (info.resident == 0 || info.lod_count == 0) continue;
        const glm::vec3 d = info.center - eye;
        order.emplace_back(glm::dot(d, d), di);
    }
    // Brief 06: CVar-backed (dbg.transp_reverse; legacy STRING_TRANSP_REVERSE alias). A/B sort check.
    if (cv_transp_reverse().get())
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });  // WRONG: nearest first
    else
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });  // farthest first (correct)

    // Brief 04c (resolution b): fill the COMPACTED per-draw list directly on the CPU in sorted order —
    // one command {ceil(LOD0 meshlets/32),1,1} + one record {draw_index, lod=0} per surviving draw,
    // in back-to-front order (the sort order == the submission order, so command-index ordering keeps
    // the blend order correct). This is the CPU analogue of the GPU compaction's fill.
    static constexpr uint32_t kTaskGroup = 32;
    char* buf = static_cast<char*>(mapped);
    auto* commands = reinterpret_cast<uint32_t*>(buf + layout_.commands_off);   // 3 uints/command
    auto* records  = reinterpret_cast<uint32_t*>(buf + layout_.records_off);    // 2 uints/record
    uint32_t count = 0;
    for (const auto& [dist2, di] : order)
    {
        if (count >= s.cull_max_draws_) break;
        const GpuDrawInfo& info = s.draw_info_mapped_[di];
        const uint32_t mcount = info.lods[0].meshlet_count;  // LOD0
        if (mcount == 0) continue;
        commands[count * 3 + 0] = (mcount + kTaskGroup - 1) / kTaskGroup;  // groupCountX
        commands[count * 3 + 1] = 1u;
        commands[count * 3 + 2] = 1u;
        records[count * 2 + 0] = di;   // draw_index
        records[count * 2 + 1] = 0u;   // lod (transparent draws all render at LOD0)
        ++count;
    }
    *reinterpret_cast<uint32_t*>(buf + layout_.count_off) = count;   // surviving-draw count
    return count;
}

// Author onto the graph. Colour write + read-only depth is the whole declaration of where this draw
// belongs: it lands in the group the opaque passes opened, as its last pass. The list buffer's two
// consumption stages are declared because the graph cannot recover them from the pass kind — the
// indirect command/count words are fetched at DRAW_INDIRECT, the records[] by the task shader.
void sorted_transparency::declare(::string::frame_graph& fg, ::string::gpu::image color,
                                  ::string::gpu::image depth, ::string::gpu::buffer list,
                                  ::string::gpu::buffer scene_data, ::string::gpu::buffer stats)
{
    fg.pass("transparency")
      .color(color)
      .depth_read(depth)
      .reads(list, access::indirect_read)
      .reads(list, access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
      // r.pass.transparency drops the blended draw -> opaque geometry only. No degrade needed:
      // transparency is a pure over-draw with no consumer.
      .toggle([] { return cv_pass_transparency().get(); })
      .raster([this, list, scene_data, stats](::string::pass_context& ctx) {
          record(ctx, list, scene_data, stats);
      });
}

void sorted_transparency::record(::string::pass_context& ctx, ::string::gpu::buffer list,
                                 ::string::gpu::buffer scene_data, ::string::gpu::buffer stats)
{
    ensure();
    if (scene_ == nullptr || program_ == nullptr || !ready_) return;
    GeometryScene& s = *scene_;
    if (s.draw_count_ == 0 || s.draw_info_mapped_ == nullptr) return;

    const uint32_t count = build_list(ctx.mapped(list));
    if (count == 0) return;

    const ::string::gpu::pipeline& tp = program_->current();
    const glm::mat4 vp = s.camera_.view_proj();

    MeshletPush push{};
    push.view_proj = vp;
    push.cull_view_proj = s.cull_enabled_ ? (s.mesh_cull_frozen_ ? s.mesh_frozen_view_proj_ : vp) : vp;
    push.vertices = allocator_->get_buffer(s.vertex_buffer_).device_address;
    push.meshlets = allocator_->get_buffer(s.meshlet_buffer_).device_address;
    push.mverts = allocator_->get_buffer(s.meshlet_vertices_).device_address;
    push.mtris = allocator_->get_buffer(s.meshlet_triangles_).device_address;
    push.draws = allocator_->get_buffer(s.draw_info_buffer_).device_address;
    push.scene = ctx.address(scene_data);
    push.stats = ctx.address(stats);
    push.records = ctx.address(list) + layout_.records_off;   // compacted {draw_index, lod=0}, back-to-front
    push.camera_pos = s.mesh_cull_frozen_ ? s.mesh_frozen_camera_pos_ : s.camera_.position();
    push.debug_view = static_cast<uint32_t>(s.debug_view_);
    // No HiZ occlusion for transparent draws: transparent surfaces are frequently coplanar with (or
    // just in front of) the opaque geometry that built the pyramid, where the conservative HiZ test
    // culls them inconsistently at the depth-equality boundary (non-deterministic pop). Transparent
    // draw counts are tiny, so skipping HiZ here costs nothing. (hiz_mips=0 -> task shader passes all.)
    push.hiz_slot = 0;
    push.hiz_mips = 0;
    push.hiz_size = glm::uvec2(1, 1);
    push.blend_pass = 1u;
    push.phase = 0u;   // transparency is single-pass (no bitfield); a valid pointer is still required
    push.bitfield = allocator_->get_buffer(s.visbits_buffer_).device_address;
    push.freeze_bits = 1u;

    ::string::gpu::command_recorder& recorder = ctx.rec;
    const VkBuffer buf = allocator_->get_buffer(ctx.id(list)).buffer;
    VkDescriptorSet mset = descriptors_->get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline_layout, 0, 1, &mset, 0, nullptr);
    recorder.push_constants(tp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    // Brief 04c (resolution b): one command per surviving transparent draw, in back-to-front order.
    recorder.draw_mesh_tasks_indirect_count(buf, layout_.commands_off, buf, layout_.count_off,
                                            s.cull_max_draws_, sizeof(uint32_t) * 3);
}

}  // namespace string::render
