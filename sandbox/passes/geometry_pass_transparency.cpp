// sorted transparency pass (CPU back-to-front) — split out of geometry_pass.cpp (brief 11 modularization). These remain GeometryPass
// member functions (cohesive translation-unit split; the geometry core stays one class, its true
// graph-pass decoupling is Phase 2). State lives in geometry_pass.hpp.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "geometry_pass.hpp"
#include "../debug_cvars.hpp"

namespace sandbox
{
using namespace String;

uint32_t GeometryPass::build_transparency_list(uint16_t current_frame)
{
    if (blend_draw_indices_.empty() || transp_mapped_.empty()) return 0;
    const glm::vec3 eye = camera_.position();

    // Sort a scratch copy back-to-front (farthest first) by squared distance eye->draw center.
    std::vector<std::pair<float, uint32_t>> order;
    order.reserve(blend_draw_indices_.size());
    for (uint32_t di : blend_draw_indices_)
    {
        if (di >= active_draw_count_) continue;
        const GpuDrawInfo& info = draw_info_mapped_[di];
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
    char* buf = reinterpret_cast<char*>(transp_mapped_[current_frame]);
    auto* commands = reinterpret_cast<uint32_t*>(buf + transp_commands_off_);   // 3 uints/command
    auto* records  = reinterpret_cast<uint32_t*>(buf + transp_records_off_);    // 2 uints/record
    uint32_t count = 0;
    for (const auto& [dist2, di] : order)
    {
        if (count >= cull_max_draws_) break;
        const GpuDrawInfo& info = draw_info_mapped_[di];
        const uint32_t mcount = info.lods[0].meshlet_count;  // LOD0
        if (mcount == 0) continue;
        commands[count * 3 + 0] = (mcount + kTaskGroup - 1) / kTaskGroup;  // groupCountX
        commands[count * 3 + 1] = 1u;
        commands[count * 3 + 2] = 1u;
        records[count * 2 + 0] = di;   // draw_index
        records[count * 2 + 1] = 0u;   // lod (transparent draws all render at LOD0)
        ++count;
    }
    *reinterpret_cast<uint32_t*>(buf + transp_count_off_) = count;   // surviving-draw count
    return count;
}

void GeometryPass::record_transparency(VkCommandBuffer cb, uint16_t current_frame, uint32_t count)
{
    if (count == 0 || !meshlet_transparent_program_) return;
    const string::gpu::pipeline& tp = meshlet_transparent_program_->current();
    const HizPyramid& hz = hiz_[current_frame];
    const bool hiz_ready = hiz_enabled_ && hz.image != 0;
    const glm::mat4 vp = camera_.view_proj();

    MeshletPush push{};
    push.view_proj = vp;
    push.cull_view_proj = cull_enabled_ ? (mesh_cull_frozen_ ? mesh_frozen_view_proj_ : vp) : vp;
    push.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
    push.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
    push.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
    push.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
    push.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    push.scene = allocator_.get_buffer(scene_buffers_[current_frame]).device_address;
    push.stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address;
    const VkDeviceAddress tbase = allocator_.get_buffer(transp_buffers_[current_frame]).device_address;
    push.records = tbase + transp_records_off_;   // compacted {draw_index, lod=0}, back-to-front order
    push.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    push.debug_view = static_cast<uint32_t>(debug_view_);
    // No HiZ occlusion for transparent draws: transparent surfaces are frequently coplanar with (or
    // just in front of) the opaque geometry that built the pyramid, where the conservative HiZ test
    // culls them inconsistently at the depth-equality boundary (non-deterministic pop). Transparent
    // draw counts are tiny, so skipping HiZ here costs nothing. (hiz_mips=0 -> task shader passes all.)
    push.hiz_slot = 0;
    push.hiz_mips = 0;
    push.hiz_size = glm::uvec2(1, 1);
    push.blend_pass = 1u;
    push.phase = 0u;   // transparency is single-pass (no bitfield); a valid pointer is still required
    push.bitfield = allocator_.get_buffer(visbits_buffer_).device_address;
    push.freeze_bits = 1u;
    (void)hiz_ready; (void)hz;

    const VkBuffer buf = allocator_.get_buffer(transp_buffers_[current_frame]).buffer;
    VkDescriptorSet mset = descriptor_table_.get_set();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline_layout, 0, 1, &mset, 0, nullptr);
    vkCmdPushConstants(cb, tp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    // Brief 04c (resolution b): one command per surviving transparent draw, in back-to-front order.
    vkCmdDrawMeshTasksIndirectCountEXT(cb, buf, transp_commands_off_, buf, transp_count_off_,
                                       cull_max_draws_, sizeof(uint32_t) * 3);
}

}  // namespace sandbox
