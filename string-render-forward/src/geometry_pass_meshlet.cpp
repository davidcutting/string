// meshlet task/mesh draw path + GPU draw-cull/expand + crowd — split out of geometry_pass.cpp (brief 11 modularization). These remain geometry_pass
// member functions (cohesive translation-unit split; the geometry core stays one class, its true
// graph-pass decoupling is Phase 2). State lives in geometry_pass.hpp.
//
// Brief 20: the cull/expand chain is DECLARED, one pass per dispatch. The four hand-rolled barriers
// that used to sit between the dispatches (cull -> expand, scan_blocks -> scan_carry, scan_carry ->
// fill, fill -> indirect draw) are gone: each pass says it writes the work-list buffer and the next
// says it reads it, and the graph derives every edge from that. Nothing in this file emits a
// hand-rolled barrier any more, and nothing in it names a pipeline-stage mask.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
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

#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace string;

void geometry_pass::build_meshlet_gpu(engine_context& context)
{
    if (meshlet_model_.total_meshlets == 0) return;

    auto make_device_buffer = [&](const void* data, VkDeviceSize size, VkBufferUsageFlags extra) {
        ::string::gpu::resource_id id = allocator_.create_resource(::string::gpu::buffer_info{
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(data, size, id);
        return id;
    };

    meshlet_buffer_ = make_device_buffer(meshlet_model_.meshlets.data(),
        sizeof(GpuMeshlet) * meshlet_model_.meshlets.size(), 0);
    meshlet_vertices_ = make_device_buffer(meshlet_model_.meshlet_vertices.data(),
        sizeof(uint32_t) * meshlet_model_.meshlet_vertices.size(), 0);
    meshlet_triangles_ = make_device_buffer(meshlet_model_.meshlet_triangles.data(),
        sizeof(uint32_t) * meshlet_model_.meshlet_triangles.size(), 0);

    // Debug (STRING_MESHLET_READBACK=1): after the transfer batch drains, read each buffer back and
    // memcmp against the CPU arrays — catches upload corruption that CPU-side validation can't see.
    if (cv_meshlet_readback().get())
    {
        meshlet_readback_pending_ = true;
    }
    if (const int32_t dump_start = cv_meshlet_dump().get(); dump_start >= 0)
    {
        const uint32_t d0 = uint32_t(dump_start);
        for (uint32_t d = d0; d < std::min<uint32_t>(d0 + 6, uint32_t(meshlet_model_.draws.size())); ++d)
        {
            // Compare the CPU model against what the GPU actually reads (mapped DrawInfo).
            if (draw_info_mapped_ && std::memcmp(&draw_info_mapped_[d], &meshlet_model_.draws[d],
                                                 sizeof(GpuDrawInfo)) != 0)
                STRING_LOG_WARN("[dump] draw {} MAPPED DrawInfo DIFFERS from CPU model!", d);
            const GpuDrawInfo& info = meshlet_model_.draws[d];
            const GpuMeshlet& m0 = meshlet_model_.meshlets[info.lods[0].meshlet_offset];
            STRING_LOG_INFO("[dump] draw {} idx_cnt {} lod0 off {} cnt {} | m0 voff {} vcnt {} tcnt {} center ({:.2f},{:.2f},{:.2f}) r {:.2f} | model[3] ({:.2f},{:.2f},{:.2f})",
                d, draws_[d].index_count, info.lods[0].meshlet_offset, info.lods[0].meshlet_count,
                m0.vertex_offset, m0.vertex_count, m0.triangle_count,
                m0.center.x, m0.center.y, m0.center.z, m0.radius,
                draws_[d].transform[3].x, draws_[d].transform[3].y, draws_[d].transform[3].z);
            // Real extent of m0's vertices straight from the meshlet-vertex remap.
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (uint32_t v = 0; v < m0.vertex_count; ++v)
            {
                const uint32_t gv = meshlet_model_.meshlet_vertices[m0.vertex_offset + v];
                // NOTE: geometry vectors moved into the streamer; use its CPU copy.
                const glm::vec3 p = geometry_streamer_->cpu_vertex(gv).pos;
                lo = glm::min(lo, p); hi = glm::max(hi, p);
            }
            STRING_LOG_INFO("[dump]   m0 REAL extent lo ({:.2f},{:.2f},{:.2f}) hi ({:.2f},{:.2f},{:.2f})",
                lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        }
    }

    // DrawInfo table (host-visible + mapped: DrawInfo.resident is the per-frame streaming gate, and
    // the crowd stress scene rewrites transforms live). Fill material/transform/bounds from draws_.
    for (std::size_t i = 0; i < meshlet_model_.draws.size(); ++i)
    {
        const GltfDraw& draw = draws_[i];
        GpuDrawInfo& info = meshlet_model_.draws[i];
        info.model = draw.transform;
        info.resident = 0;       // flipped on residency (up-front loop / streaming)
        info.skinned = 0;        // Phase B reserves this (bounds inflation + cone-cull bypass)
        info.flags = 0;
        info.alpha_cutoff = 0.5f;
        if (draw.material >= 0)
        {
            const GltfMaterial& m = materials_[draw.material];
            info.base_color = m.base_color_factor;
            info.base_slot = m.base_color_texture >= 0 ? texture_slots_[m.base_color_texture] : white_slot_;
            info.normal_slot = m.normal_texture >= 0 ? texture_slots_[m.normal_texture] : flat_normal_slot_;
            info.mr_slot = m.metallic_roughness_texture >= 0 ? texture_slots_[m.metallic_roughness_texture] : white_slot_;
            info.metallic = m.metallic_factor;
            info.roughness = m.roughness_factor;
            info.occlusion_slot = m.occlusion_texture >= 0 ? texture_slots_[m.occlusion_texture] : white_slot_;
            // Brief 04: MASK => alpha-tested cutout; BLEND => routed to the sorted transparency pass
            // (excluded from the opaque lists); doubleSided => two-sided (bypass cull both places).
            if (m.alpha_mode == GltfAlphaMode::Mask)  info.flags |= kDrawFlagCutout;
            if (m.alpha_mode == GltfAlphaMode::Blend) info.flags |= kDrawFlagBlend;
            if (m.double_sided)                       info.flags |= kDrawFlagDoubleSided;
            info.alpha_cutoff = m.alpha_cutoff;
        }
        else
        {
            info.base_color = glm::vec4(1.0f);
            info.base_slot = white_slot_;
            info.normal_slot = flat_normal_slot_;
            info.mr_slot = white_slot_;
            info.metallic = 1.0f;
            info.roughness = 1.0f;
            info.occlusion_slot = white_slot_;
        }
        // Brief 20: the BLEND draw list is built by sorted_transparency from the same flags.
    }

    // Size the DrawInfo table for base + the full crowd grid (extra copies referencing the same
    // meshlets, only the transform differs). Crowd entries are filled/cleared on the K toggle.
    base_draw_count_ = static_cast<uint32_t>(meshlet_model_.draws.size());
    active_draw_count_ = base_draw_count_;
    const uint32_t max_draws = base_draw_count_ * kCrowdGrid * kCrowdGrid;
    const VkDeviceSize draw_info_size = sizeof(GpuDrawInfo) * max_draws;
    draw_info_buffer_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = draw_info_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    draw_info_mapped_ = static_cast<GpuDrawInfo*>(
        allocator_.get_buffer(draw_info_buffer_).allocation_info.pMappedData);
    std::copy(meshlet_model_.draws.begin(), meshlet_model_.draws.end(), draw_info_mapped_);

    // GPU-written stats, read back one frame late. Brief 20: the stats ring is an APP-DECLARED graph
    // buffer; the passes that write it declare it and resolve its address through pass_context.
    stats_readback_.resize(frames_in_flight_);

    // --- Brief 04c (resolution b): GPU meshlet worklist buffers (per frame in flight). Each worklist
    // packs, at 16B-aligned regions: counts[max_draws] (uint), offsets[max_draws] (uint, scratch),
    // block_sums[max_blocks] (uint, scratch), commands[max_draws] (12B VkDrawMeshTasksIndirectCommandEXT),
    // records[max_draws] (8B {draw_index, lod}), then the surviving-draw count word. The draw phase
    // writes counts (+ the shared draw_lod buffer); the compaction phase scans the survival predicate
    // and fills the DENSE commands[]+records[]+count in ascending draw order; one
    // vkCmdDrawMeshTasksIndirectCountEXT consumes it (command ordering pins coplanar winners).
    cull_max_draws_ = max_draws;
    cull_max_blocks_ = (max_draws + kScanBlock - 1) / kScanBlock;

    const VkDeviceSize u32 = sizeof(uint32_t);
    const auto align16 = [](VkDeviceSize v) { return (v + 15) & ~VkDeviceSize(15); };
    const VkDeviceSize counts_bytes    = u32 * max_draws;
    const VkDeviceSize offsets_bytes   = u32 * max_draws;
    const VkDeviceSize blocksums_bytes = u32 * cull_max_blocks_;
    const VkDeviceSize commands_bytes  = VkDeviceSize(sizeof(uint32_t) * 3) * max_draws;   // 12B/cmd
    const VkDeviceSize records_bytes   = VkDeviceSize(sizeof(uint32_t) * 2) * max_draws;   // 8B/record
    // Shared with ShadowMaps, which reserves its own per-cascade regions of the same shape.
    wl_layout_.offsets_off   = align16(counts_bytes);
    wl_layout_.blocksums_off = align16(wl_layout_.offsets_off + offsets_bytes);
    wl_layout_.commands_off  = align16(wl_layout_.blocksums_off + blocksums_bytes);
    wl_layout_.records_off   = align16(wl_layout_.commands_off + commands_bytes);
    wl_layout_.count_off     = align16(wl_layout_.records_off + records_bytes);
    const VkDeviceSize worklist_size = wl_layout_.count_off + 16;   // count word (16B-padded)
    wl_layout_.size = worklist_size;

    // Brief 21 D4: the work lists are graph TRANSIENTS the app declares (one buffer each, sized from
    // worklist_bytes()/draw_lod_bytes() below) and hands to declare(). Nothing is reserved, bound or
    // materialized here any more — that whole dance was the scratch arena working around the graph
    // not owning its own transients.
    (void)worklist_size;

    // Brief 04d: persistent per-meshlet visibility bitfield (1 bit per GLOBAL meshlet id). Sized to
    // total meshlet capacity, zero-initialized (a cleared bit -> that meshlet takes phase 2 for one
    // frame — the correct warmup/fallback/streaming-change behaviour). Device-local, NOT ring-buffered:
    // the temporal state accumulates across frames. TRANSFER_DST for the vkCmdFillBuffer clears.
    visbits_words_ = (meshlet_model_.total_meshlets + 31u) / 32u;
    visbits_buffer_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = VkDeviceSize(u32) * std::max(1u, visbits_words_),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Per-draw LOD selected LAST frame (for the LOD-switch bit invalidation). Zeroed at first clear.
    prev_draw_lod_buffer_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = u32 * max_draws,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // Brief 20: the sorted-transparency list buffers are owned by sorted_transparency, which builds
    // them in the same compacted shape from the same BLEND flags.

    // --- Pipelines (all through the hot-reload registry) ---
    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    VkSampleCountFlagBits samples = scene_samples_;

    // Task/mesh/fragment lit meshlet pipeline. Same fixed state as the lit vertex pipeline (back-face
    // cull, CCW, MSAA, reverse-Z depth, alpha blend), but built via build_mesh_pipeline (no VI state).
    // Brief 04: two variants off the same source differ only in fixed state:
    //   - one-sided opaque: CULL_BACK, depth write ON;
    //   - two-sided opaque: CULL_NONE, depth write ON.
    // (The transparency variant belongs to sorted_transparency now.)
    const auto make_lit = [layout, samples](VkCullModeFlags cull_mode, bool depth_write) {
        return [layout, samples, cull_mode, depth_write](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
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
                .set_rasterization(VK_POLYGON_MODE_FILL, cull_mode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_multisampling(samples)
                .enable_depth_stencil(/*depth_test*/ true, depth_write, VK_COMPARE_OP_GREATER_OR_EQUAL)
                .enable_color_blending()
                .build_mesh_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        };
    };
    meshlet_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "meshlet_mesh.slang", make_lit(VK_CULL_MODE_BACK_BIT, /*depth_write*/ true));
    meshlet_twosided_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "meshlet_mesh.slang", make_lit(VK_CULL_MODE_NONE, /*depth_write*/ true));

    // The depth-only meshlet shadow pipeline belongs to shadow_maps now (brief 20).

    // HiZ pyramid downsample + stats reset compute pipelines.
    auto make_compute = [&](const char* file) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / file,
            [layout](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
                ::string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = ::string::gpu::pipeline_type::COMPUTE;
                return p;
            });
    };
    // Brief 04c: the expansion shader has three compute entry points (scan_blocks / scan_carry /
    // fill). Select one by name so each becomes its own pipeline through the same hot-reload registry.
    auto make_compute_entry = [&](const char* file, const char* entry) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / file,
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
    hiz_program_ = make_compute("hiz_build.slang");
    reset_program_ = make_compute("meshlet_reset.slang");
    draw_cull_program_ = make_compute("meshlet_draw_cull.slang");
    expand_scan_blocks_program_ = make_compute_entry("meshlet_expand.slang", "scan_blocks");
    expand_scan_carry_program_ = make_compute_entry("meshlet_expand.slang", "scan_carry");
    expand_fill_program_ = make_compute_entry("meshlet_expand.slang", "fill");

    // Brief 20: the hand-rolled HiZ sampler is gone. The pyramid is an application-declared graph
    // image whose sampler is configured on the declaration (nearest + clamp), so `bind()` is enough
    // and this file never creates a VkSampler.
    hiz_.resize(frames_in_flight_);
}

void geometry_pass::build_crowd(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
{
    if (!draw_info_mapped_) return;
    if (!crowd_enabled_)
    {
        active_draw_count_ = base_draw_count_;
        return;
    }
    const glm::vec3 extent = aabb_max - aabb_min;
    const float spacing_x = extent.x * 1.1f;
    const float spacing_z = extent.z * 1.1f;
    const int side = static_cast<int>(kCrowdGrid);   // kCrowdGrid x kCrowdGrid cells total
    uint32_t dst = base_draw_count_;
    for (int gx = 0; gx < side; ++gx)
    for (int gz = 0; gz < side; ++gz)
    {
        // Center the grid on the origin cell (occupied by the base scene).
        const int cx = gx - side / 2;
        const int cz = gz - side / 2;
        if (cx == 0 && cz == 0) continue;   // the base scene occupies the origin cell
        const glm::mat4 offset = glm::translate(glm::mat4(1.0f),
            glm::vec3(static_cast<float>(cx) * spacing_x, 0.0f, static_cast<float>(cz) * spacing_z));
        for (uint32_t b = 0; b < base_draw_count_; ++b)
        {
            GpuDrawInfo copy = meshlet_model_.draws[b];   // geometry-only record (bounds + LOD ranges)
            const GpuDrawInfo& base_live = draw_info_mapped_[b];  // material/transform/resident live
            copy.model = offset * base_live.model;
            // DrawInfo.center is WORLD-space (draw-level frustum cull + LOD select read it raw), so
            // the grid translation must be applied to the copy's bounds too.
            copy.center = glm::vec3(offset[3]) + copy.center;
            copy.base_color = base_live.base_color;
            copy.base_slot = base_live.base_slot;
            copy.normal_slot = base_live.normal_slot;
            copy.mr_slot = base_live.mr_slot;
            copy.metallic = base_live.metallic;
            copy.roughness = base_live.roughness;
            copy.occlusion_slot = base_live.occlusion_slot;
            copy.resident = base_live.resident;
            draw_info_mapped_[dst++] = copy;
        }
    }
    active_draw_count_ = dst;
    STRING_LOG_INFO("[crowd] {} draws active ({} base x {} grid cells)",
                    active_draw_count_, base_draw_count_, kCrowdGrid * kCrowdGrid);
}

// --- Brief 20: the cull/expand chain as declarations ----------------------------------------------
//
// Is the meshlet culler able to run at all this frame? Folded into every conditional in this file, so
// a scene that has not finished loading SKIPS these passes instead of dispatching over nothing — and
// their consumers degrade through the graph rather than through a hand-written guard.
namespace
{
constexpr uint32_t kListOpaque = 0;
constexpr uint32_t kListTwosided = 1;
constexpr uint32_t kListCascade0 = 2;   // + cascade index
}  // namespace

// One declared pass per draw-cull dispatch: the camera list, then one per cascade. Each WRITES the
// work-list buffer and the stats buffer; the expand passes below READ the work-list buffer, and that
// pair of declarations is what replaced the hand-rolled memory barrier at the tail of the old
// record_draw_cull (counts + draw_lod -> the expansion compute's storage read).
string::gpu::buffer geometry_pass::list_of(uint32_t list) const
{
    if (list == kListOpaque) return wl_.opaque;
    if (list == kListTwosided) return wl_.twosided;
    const uint32_t cascade = list - kListCascade0;
    return cascade < wl_.cascade.size() ? wl_.cascade[cascade] : string::gpu::buffer{};
}

void geometry_pass::declare_cull(string::frame_graph& fg, string::gpu::buffer stats)
{
    const auto ready = [this] {
        return meshlet_program_ != nullptr && draw_info_mapped_ != nullptr
            && draw_cull_program_ != nullptr && expand_fill_program_ != nullptr
            && active_draw_count_ > 0;
    };

    // The camera dispatch writes BOTH camera lists and the shared draw_lod — three declarations,
    // because it is three buffers. Naming them individually is what lets the graph order the cascade
    // chains independently of the camera's instead of serialising everything through one arena.
    fg.pass("meshlet.cull.camera")
      .writes(wl_.opaque)
      .writes(wl_.twosided)
      .writes(wl_.draw_lod)
      .writes(stats)
      .toggle(ready)
      .compute([this, stats](string::pass_context& ctx) {
          record_draw_cull(ctx, stats, /*cascade*/ -1);
      });

    // One pass per POSSIBLE cascade: the declaration count is fixed (authored once), and a cascade the
    // scene does not have is an in-graph conditional that is simply false. A cascade dispatch writes
    // ONLY its own list — it deliberately does not re-write draw_lod (see record_draw_cull).
    for (uint32_t c = 0; c < kMaxCascades; ++c)
    {
        fg.pass("meshlet.cull.shadow" + std::to_string(c))
          .writes(wl_.cascade[c])
          .writes(stats)
          .toggle([this, ready, c] {
              return ready() && shadow != nullptr && c < shadow->cascade_count();
          })
          .compute([this, stats, c](string::pass_context& ctx) {
              record_draw_cull(ctx, stats, static_cast<int>(c));
          });
    }
}

// Three declared passes per work list — scan_blocks, scan_carry, fill — because that is three
// dependent dispatches over one buffer, and the two barriers that used to sit between them inside
// record_expand are exactly what `.reads(worklists).writes(worklists)` derives. The trailing barrier
// (fill -> the indirect draw + the task shader's records[] read) is derived from the draw passes'
// own `.reads(worklists, indirect_read)` / `.reads(worklists, storage_read, TASK)` declarations.
//
// Each stage names ITS OWN list's buffer, so the three chains (camera, two-sided, each cascade) are
// independent in the graph and no longer serialise against each other through a shared arena.
void geometry_pass::declare_expand(string::frame_graph& fg)
{
    const auto ready = [this] {
        return meshlet_program_ != nullptr && draw_info_mapped_ != nullptr
            && draw_cull_program_ != nullptr && expand_fill_program_ != nullptr
            && active_draw_count_ > 0;
    };

    const auto declare_list = [&](uint32_t list, const std::string& name, enable_fn on) {
        const string::gpu::buffer b = list_of(list);
        // Every stage also READS draw_lod: the records[] it fills inherit the camera-selected LOD.
        fg.pass("meshlet.expand." + name + ".scan_blocks")
          .reads(b).writes(b).reads(wl_.draw_lod)
          .toggle(on)
          .compute([this, b](string::pass_context& ctx) { record_expand_scan_blocks(ctx, b); });
        fg.pass("meshlet.expand." + name + ".scan_carry")
          .reads(b).writes(b).reads(wl_.draw_lod)
          .toggle(on)
          .compute([this, b](string::pass_context& ctx) { record_expand_scan_carry(ctx, b); });
        fg.pass("meshlet.expand." + name + ".fill")
          .reads(b).writes(b).reads(wl_.draw_lod)
          .toggle(on)
          .compute([this, b](string::pass_context& ctx) { record_expand_fill(ctx, b); });
    };

    declare_list(kListOpaque, "opaque", ready);
    declare_list(kListTwosided, "twosided", ready);
    for (uint32_t c = 0; c < kMaxCascades; ++c)
        declare_list(kListCascade0 + c, "shadow" + std::to_string(c),
                     [this, ready, c] {
                         return ready() && shadow != nullptr && c < shadow->cascade_count();
                     });
}

void geometry_pass::record_draw_cull(::string::pass_context& ctx, ::string::gpu::buffer stats,
                                     int cascade)
{
    // Brief 04c DRAW PHASE. cascade < 0: camera — writes the opaque + two-sided per-draw meshlet
    // counts (into wl_opaque/wl_twosided counts@0) + the shared draw_lod. cascade >= 0: that
    // cascade's shadow list — writes counts_shadow into the cascade list's counts@0, rejecting whole
    // draws outside the CASCADE's light sphere (never the camera frustum).
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const bool shadow_pass = cascade >= 0;
    if (shadow_pass && (shadow == nullptr || uint32_t(cascade) >= shadow->cascade_count())) return;

    const VkDeviceAddress opaque_base = ctx.address(wl_.opaque);
    const VkDeviceAddress twosided_base = ctx.address(wl_.twosided);
    const VkDeviceAddress shadow_base = shadow_pass
        ? ctx.address(list_of(kListCascade0 + uint32_t(cascade))) : 0;
    if (opaque_base == 0 || twosided_base == 0 || (shadow_pass && shadow_base == 0)) return;

    const float lod_error_px = cv_lod_error_px().get();   // CVar r.lod.error_px (STRING_LOD_PX alias)
    const float half_h = screen_size.height * 0.5f;
    const float focal = half_h / std::tan(glm::radians(camera_.fov_degrees() * 0.5f));

    const ::string::gpu::pipeline& cp = draw_cull_program_->current();
    DrawCullPush cull{};
    // Freeze-aware inputs: when F is held EVERY camera-derived cull input comes from the frozen pose.
    cull.cull_view_proj = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : camera_.view_proj();
    cull.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    if (shadow_pass)
    {
        // Shadow only consumes counts_shadow (counts@0 of the cascade list); the opaque/two-sided
        // writes land in this same region's scratch (overwritten by its own expand before use).
        cull.counts_shadow    = shadow_base;                              // counts@0
        cull.counts           = shadow_base + wl_layout_.offsets_off;     // throwaway (expand recomputes)
        cull.counts_twosided  = shadow_base + wl_layout_.blocksums_off;   // throwaway
    }
    else
    {
        cull.counts          = opaque_base;      // counts@0 of the opaque worklist
        cull.counts_twosided = twosided_base;    // counts@0 of the two-sided worklist
        cull.counts_shadow   = opaque_base + wl_layout_.offsets_off;   // throwaway (camera ignores it)
    }
    // Only the CAMERA opaque/two-sided pass writes the shared camera-selected draw_lod (the main pass +
    // shadow compaction records read it). The shadow passes must NOT re-write it — a redundant second
    // writer creates a write-after-write hazard on the shared buffer for no benefit (shadow compaction
    // reuses the camera draw_lod). Shadow writes its throwaway per-draw LODs into a scratch region (the
    // commands[] area, overwritten by its own fill before use) instead.
    cull.draw_lod = shadow_pass
        ? shadow_base + wl_layout_.commands_off   // shadow throwaway -> commands (fill overwrites)
        : ctx.address(wl_.draw_lod);
    cull.stats = ctx.address(stats);
    cull.draw_count = active_draw_count_;
    cull.lod_enabled = lod_enabled_ ? 1u : 0u;
    cull.force_lod0 = 0u;                                       // (dead: brief 04d deleted the LOD0 prepass)
    cull.isolate_draw = cv_isolate_draw().get();               // brief 06: >=0 keeps only that draw
    cull.frustum_cull = cull_enabled_ ? 1u : 0u;              // C toggle disables DRAW-level camera cull
    cull.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    cull.lod_error_px = lod_error_px;
    cull.focal = focal;
    // Histogram only on the camera opaque/twosided pass (not shadow) — matches pre-04c meaning.
    cull.stats_lod = shadow_pass ? 0u : 1u;
    cull.shadow_mode = (shadow_pass && cull_enabled_) ? 1u : 0u;
    if (shadow_pass)
    {
        cull.shadow_center = cascade_center_[cascade];
        cull.shadow_radius = cascade_cull_radius_[cascade];
    }
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, cp.pipeline);
    recorder.push_constants(cp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(DrawCullPush), &cull);
    recorder.dispatch((active_draw_count_ + 63) / 64, 1, 1);

    // NO barrier. counts[] + draw_lod reaching the expansion compute is derived from this pass's
    // declared write of `worklists` against meshlet.expand.*.scan_blocks's declared read of it.
}

namespace
{
// The push every expand stage shares. Every address is resolved from a DECLARED handle at record
// time — the pass owns handles, never addresses.
ExpandPush make_expand_push(::string::pass_context& ctx, ::string::gpu::buffer list,
                            const WorklistLayout& layout, ::string::gpu::buffer draw_lod,
                            uint32_t draw_count, uint32_t max_draws)
{
    const VkDeviceAddress base = ctx.address(list);
    ExpandPush push{};
    push.counts      = base;
    push.offsets     = base + layout.offsets_off;
    push.block_sums  = base + layout.blocksums_off;
    push.draw_lod    = ctx.address(draw_lod);
    push.commands    = base + layout.commands_off;
    push.records     = base + layout.records_off;
    push.count       = base + layout.count_off;
    push.draw_count  = draw_count;
    push.block_count = (draw_count + kScanBlock - 1) / kScanBlock;
    push.max_draws   = max_draws;
    return push;
}
}  // namespace

// Stage 1: one workgroup per block. Was the first dispatch of record_expand; the barrier that
// followed it is now the scan_carry pass's declared read of the same buffer.
void geometry_pass::record_expand_scan_blocks(::string::pass_context& ctx, ::string::gpu::buffer list)
{
    if (ctx.address(list) == 0) return;
    const ExpandPush push = make_expand_push(ctx, list, wl_layout_, wl_.draw_lod,
                                             active_draw_count_, cull_max_draws_);
    const ::string::gpu::pipeline& p = expand_scan_blocks_program_->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    ctx.rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ExpandPush), &push);
    ctx.rec.dispatch(push.block_count, 1, 1);
}

// Stage 2: the single-workgroup block-carry scan.
void geometry_pass::record_expand_scan_carry(::string::pass_context& ctx, ::string::gpu::buffer list)
{
    if (ctx.address(list) == 0) return;
    const ExpandPush push = make_expand_push(ctx, list, wl_layout_, wl_.draw_lod,
                                             active_draw_count_, cull_max_draws_);
    const ::string::gpu::pipeline& p = expand_scan_carry_program_->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    ctx.rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ExpandPush), &push);
    ctx.rec.dispatch(1, 1, 1);
}

// Stage 3: fill the dense commands[]/records[]/count. The barrier that used to follow this — making
// them visible to the indirect draw and the task shader — is derived from the DRAW passes' declared
// reads of the same buffer (indirect_read, and storage_read at the task stage).
void geometry_pass::record_expand_fill(::string::pass_context& ctx, ::string::gpu::buffer list)
{
    if (ctx.address(list) == 0) return;
    const ExpandPush push = make_expand_push(ctx, list, wl_layout_, wl_.draw_lod,
                                             active_draw_count_, cull_max_draws_);
    const ::string::gpu::pipeline& p = expand_fill_program_->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    ctx.rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ExpandPush), &push);
    ctx.rec.dispatch((active_draw_count_ + 63) / 64, 1, 1);
}

void geometry_pass::record_meshlet_draws(::string::pass_context& ctx, const ::string::gpu::pipeline& p,
                                         ::string::gpu::buffer list, uint32_t phase,
                                         ::string::gpu::image pyramid,
                                         ::string::gpu::buffer scene_data, ::string::gpu::buffer stats)
{
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const uint32_t current_frame = ctx.frame_slot;
    const ::string::gpu::resource_id wl_id = ctx.id(list);
    if (wl_id == 0) return;
    if (current_frame >= hiz_.size()) return;

    const glm::mat4 vp = camera_.view_proj();
    const glm::mat4 cull_vp = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : vp;
    const HizPyramid& hz = hiz_[current_frame];
    const bool hiz_ready = hiz_enabled_ && hz.mips != 0;
    const VkDeviceAddress base = ctx.address(list);

    MeshletPush push{};
    push.view_proj = vp;
    push.cull_view_proj = cull_enabled_ ? cull_vp : vp;   // (frustum planes; C disables via wide test)
    // The geometry heaps are NOT graph resources — they are load-time device buffers this pass owns,
    // so they keep resolving through the allocator. Only the declared handles go through ctx.
    push.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
    push.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
    push.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
    push.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
    push.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    push.scene = ctx.address(scene_data);
    push.stats = ctx.address(stats);
    push.records = base + wl_layout_.records_off;
    // Frozen-aware eye: cone backface + HiZ nearest-point tests must use the SAME eye the frustum
    // froze from, or freeze-cull mixes live/frozen inputs (visible as bogus culling when flying).
    push.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    push.debug_view = static_cast<uint32_t>(debug_view_);
    // Brief 04d: phase 1 renders bit-set (last-frame-visible) meshlets with NO HiZ test (the pyramid
    // isn't built yet); phase 2 tests the bit-clear complement against the freshly-built pyramid. Phase
    // 0 is the legacy single-pass (HiZ-off) path. Only phase 2 needs the pyramid slot.
    const bool use_hiz = hiz_ready && phase != 1u;
    push.hiz_slot = use_hiz ? ctx.slot(pyramid) : 0;
    push.hiz_mips = use_hiz ? hz.mips : 0;
    push.hiz_size = use_hiz ? hz.size : glm::uvec2(1, 1);
    push.blend_pass = 0u;
    push.phase = phase;
    push.bitfield = allocator_.get_buffer(visbits_buffer_).device_address;
    push.freeze_bits = mesh_cull_frozen_ ? 1u : 0u;
    const VkBuffer buf = allocator_.get_buffer(wl_id).buffer;

    recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    recorder.draw_mesh_tasks_indirect_count(buf, wl_layout_.commands_off,
                                            buf, wl_layout_.count_off,
                                            cull_max_draws_, sizeof(uint32_t) * 3);
}

void geometry_pass::record_opaque_phase(::string::pass_context& ctx, uint32_t phase,
                                        ::string::gpu::image pyramid,
                                        ::string::gpu::buffer scene_data, ::string::gpu::buffer stats)
{
    // HANG GUARD. This issues draw_mesh_tasks_indirect_count against command records the CULL and
    // EXPAND passes fill. Those are gated on readiness; this pass is gated only on r.pass.geometry.
    // If the draw ran while they had not, it would read whatever the arena happened to contain and
    // hand the driver an arbitrary task-group count — which is a GPU hang, then a TDR, then a lost
    // device. maxDrawCount bounds the number of commands, NOT the group count inside one.
    // The predicate below is deliberately the SAME one declare_cull/declare_expand toggle on.
    if (meshlet_program_ == nullptr || !draw_info_mapped_ || active_draw_count_ == 0) return;

    ::string::gpu::command_recorder& recorder = ctx.rec;
    const ::string::gpu::pipeline& mp = meshlet_program_->current();
    VkDescriptorSet mset = descriptor_table_.get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mp.pipeline_layout, 0, 1, &mset, 0, nullptr);
    record_meshlet_draws(ctx, mp, wl_.opaque, phase, pyramid, scene_data, stats);
    if (meshlet_twosided_program_)
    {
        const ::string::gpu::pipeline& tp = meshlet_twosided_program_->current();
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      tp.pipeline_layout, 0, 1, &mset, 0, nullptr);
        record_meshlet_draws(ctx, tp, wl_.twosided, phase, pyramid, scene_data, stats);
    }
}

}  // namespace string::render
