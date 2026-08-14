// Brief 09b probe GI (relightable irradiance volume). Brief 20: the component owns its five
// pipelines and its amortization state and NOTHING ELSE — the atlases, the cube G-buffer, the
// classification/relocation buffers and the capture table are graph resources the application
// declares, and the nine hand-rolled barriers are gone, replaced by seven declared passes whose
// edges the graph derives. See probe_gi_component.hpp for the pass list and why the capture/collapse
// pair is unrolled per probe slot.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <glm/gtc/constants.hpp>

#include <string/render/probe_gi_component.hpp>
#include <string/render/geometry/ibl_component.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace string;

// --- the grid fit (pure; the app sizes its declarations with it) ---------------------------------

// Fit a uniform probe grid to the scene AABB using the spacing CVar. Counts are clamped per-axis and
// by total (atlas VRAM / relight cost guard).
ProbeVolume probe_gi_component::fit_volume(glm::vec3 aabb_min, glm::vec3 aabb_max)
{
    ProbeVolume v{};
    const glm::vec3 ext = aabb_max - aabb_min;
    if (ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f)
    {
        v.valid = false;
        return v;
    }
    float spacing = cv_gi_spacing().get();
    if (spacing <= 0.0f)
    {
        // Auto: aim for ~16 probes along the longest axis.
        const float longest = std::max(ext.x, std::max(ext.y, ext.z));
        spacing = std::max(longest / 16.0f, 0.25f);
    }
    // Anisotropic spacing: architectural scenes vary less vertically than horizontally, and a coarser
    // Y is a cheap probe-count win — while still keeping >=2 layers in any room.
    glm::vec3 sp{ spacing, spacing * 1.5f, spacing };

    // Grid FIXED-spacing, fit to a TRUE half-cell inset of the AABB: geometry sits at axis-aligned
    // positions, so a grid flush to the AABB plants probes exactly in floors/walls/column axes. The
    // outermost probe layer on every axis lands within half a cell INSIDE the AABB (never outside
    // it). Trilinear clamps at the volume edge, so boundary surfaces (floor, outer walls) sample the
    // nearest in-volume layer.
    const auto axis_count = [&](float e, float s) {
        return std::clamp(uint32_t(std::ceil(e / s)), 2u, kProbeMaxPerAxis);
    };
    glm::uvec3 counts{ axis_count(ext.x, sp.x), axis_count(ext.y, sp.y), axis_count(ext.z, sp.z) };
    while (counts.x * counts.y * counts.z > kProbeMaxTotal)
    {
        sp *= 1.25f;   // too many probes -> coarsen uniformly and retry
        counts = glm::uvec3{ axis_count(ext.x, sp.x), axis_count(ext.y, sp.y), axis_count(ext.z, sp.z) };
    }
    // Centred inset origin: the probe lattice (span `grid_span` <= ext) is centred in the AABB, so
    // the leftover `ext - grid_span` (in [0, sp]) splits evenly and both end layers sit within half a
    // cell INSIDE their faces — symmetric, hugging neither min nor max.
    const glm::vec3 grid_span = glm::vec3(counts - glm::uvec3(1u)) * sp;
    v.origin = aabb_min + (ext - grid_span) * 0.5f;
    v.spacing = sp;
    v.counts = counts;
    v.valid = true;
    return v;
}

// The flat capture -> {draw index, global meshlet id} table, built ONCE from the meshlet model so the
// capture task shader resolves a flat dispatch index to its draw + real global meshlet id with no CPU
// per-draw loop.
//
// The capture uses each draw's COARSEST LOD, not LOD0: the cube faces are 32x32 px, so full-detail
// geometry is pure waste — the bake cost is dominated by (meshlets x 64 vertex transforms) per probe,
// and the coarse LOD cuts the meshlet count ~an order of magnitude (Sponza: 122k LOD0 meshlets over
// 450 draws). GI capture on proxy/low-LOD geometry is the shipped-engine standard; the slight surface
// shift is far below the probe grid's spatial resolution.
std::vector<glm::uvec2> probe_gi_component::capture_table(std::span<const GpuDrawInfo> draws)
{
    std::vector<glm::uvec2> mdraw;
    for (uint32_t d = 0; d < draws.size(); ++d)
    {
        const GpuDrawInfo& di = draws[d];
        if (di.lod_count == 0) continue;
        // Brief 23: skinned draws are EXCLUDED — this capture is a static per-probe bake, and a
        // character baked in bind pose would ghost into the probes forever. (build_meshlet_gpu
        // sets the flag before this table is built; the raster shader also passes a null skin
        // context, belt and braces.)
        if (di.skinned != 0) continue;
        const uint32_t coarse = di.lod_count - 1;
        const uint32_t off = di.lods[coarse].meshlet_offset;
        const uint32_t cnt = di.lods[coarse].meshlet_count;
        for (uint32_t m = 0; m < cnt; ++m) mdraw.push_back({ d, off + m });
    }
    return mdraw;
}

namespace
{

// The collapse/clear push mirrors probe_capture.slang's Push exactly (std430; offsets verified
// against %Push_std430 OpMemberDecorate: probe_pos@48, probe_index@60, active ptr@64).
struct ProbeCapturePush
{
    uint32_t probe_base, probe_count, counts_x, counts_y, counts_z;
    uint32_t cap_gbuf_slot, cap_albedo_slot, vis_slot;
    float far_distance;
    uint32_t cube_albedo_slot, cube_nd_slot;
    uint32_t round2;         // 44: bake round (0 = relocate, 1 = re-capture from relocated pos)
    glm::vec3 probe_pos;
    uint32_t probe_index;
    VkDeviceAddress active;
    float _pad1;             // 72 (pad so spacing lands on its 16B boundary)
    float _pad2;             // 76
    glm::vec3 spacing;       // 80 (relocation clamp)
    float _pad3;             // 92
    VkDeviceAddress offset;  // 96 (per-probe relocation output)
};
static_assert(offsetof(ProbeCapturePush, probe_pos) == 48);
static_assert(offsetof(ProbeCapturePush, probe_index) == 60);
static_assert(offsetof(ProbeCapturePush, active) == 64);
static_assert(offsetof(ProbeCapturePush, spacing) == 80);
static_assert(offsetof(ProbeCapturePush, offset) == 96);

// The per-face cube basis + view-projection live IN probe_capture_raster.slang (face_view_proj /
// kFaceF/R/U), built from probe_pos so the push stays tiny (6 mat4 would blow the 256B budget). The
// convention mirrors ibl.slang face_dir() so the collapse SamplerCube reads agree with the writes.
struct ProbeRasterPush
{
    VkDeviceAddress vertices, meshlets, mverts, mtris, draws, mdraw;
    glm::vec3 probe_pos; uint32_t meshlet_count;
    float cull_far; uint32_t probe_index;
    VkDeviceAddress offsets;   // relocation (raster adds offset[probe_index] GPU-side)
};
static_assert(offsetof(ProbeRasterPush, probe_pos) == 48);
static_assert(offsetof(ProbeRasterPush, meshlet_count) == 60);
static_assert(offsetof(ProbeRasterPush, cull_far) == 64);
static_assert(offsetof(ProbeRasterPush, probe_index) == 68);
static_assert(offsetof(ProbeRasterPush, offsets) == 72);
static_assert(sizeof(ProbeRasterPush) == 80);

glm::vec4 probe_origin_spacing(const ProbeVolume& v) { return glm::vec4(v.origin, v.spacing.x); }

}  // namespace

// --- construction --------------------------------------------------------------------------------

probe_gi_component::probe_gi_component(engine_context& ctx, GeometryScene* scene,
                                       VkSampleCountFlagBits scene_samples)
: device_(&ctx.device)
, allocator_(&ctx.allocator)
, descriptor_set_(ctx.descriptor_table.get_set())
, scene_(scene)
{
    probe_volume_ = fit_volume(scene_->scene_aabb_min_, scene_->scene_aabb_max_);

    // ANALYTIC capture distance-cull radius (derived, not tuned). The visibility atlas only ever
    // answers Chebyshev queries from shading points inside a probe's ADJACENT cells (the 8-probe
    // trilinear samples the enclosing cell's corners), so the largest distance that must be captured
    // accurately is exactly:
    //     r_vis = |spacing|            worst-case shading point at the opposite cell corner
    //           + 0.5 * max(spacing)   relocation can move the probe up to half a cell
    //           + 0.75 * min(spacing)  the DDGI normal/view sampling bias applied at shading
    // Geometry beyond r_vis can read as "far" without changing ANY visibility result, and the radius
    // scales with the grid: tighter spacing -> tighter radius -> cheaper bake, automatically.
    // Radiance caveat (documented trade-off): the M2 relight treats first-hits beyond the radius as
    // open sky. Acceptable for Sponza-class scenes.
    if (probe_volume_.valid)
    {
        const glm::vec3 ext = scene_->scene_aabb_max_ - scene_->scene_aabb_min_;
        const glm::vec3 sp = probe_volume_.spacing;
        const float r_vis = glm::length(sp) + 0.5f * std::max(sp.x, std::max(sp.y, sp.z))
                          + 0.75f * std::min(sp.x, std::min(sp.y, sp.z));
        probe_cull_far_ = std::min(glm::length(ext), r_vis);

        const glm::uvec2 vtiles = probe_volume_.tile_grid();
        const uint32_t irrad_w = vtiles.x * kProbeIrradStride, irrad_h = vtiles.y * kProbeIrradStride;
        const uint32_t vis_w = vtiles.x * kProbeVisStride, vis_h = vtiles.y * kProbeVisStride;
        const double mb = (double(irrad_w) * irrad_h * 8.0             // irradiance RGBA16F
                           + double(vis_w) * vis_h * 8.0               // visibility
                           + double(vis_w) * vis_h * 8.0 * 2.0)        // capture gbuf + albedo
                          / (1024.0 * 1024.0);
        STRING_LOG_INFO("[gi] probe grid {}x{}x{} = {} probes, spacing ({:.2f},{:.2f},{:.2f}) m, "
                        "irrad atlas {}x{}, vis atlas {}x{}, ~{:.2f} MB",
                        probe_volume_.counts.x, probe_volume_.counts.y, probe_volume_.counts.z,
                        probe_volume_.total(), sp.x, sp.y, sp.z,
                        irrad_w, irrad_h, vis_w, vis_h, mb);
    }

    VkDescriptorSetLayout layout = ctx.descriptor_table.get_layout();
    const auto make_probe_compute = [&](const char* file, const char* entry) {
        return ctx.shader_registry.create(
            ctx.resources_path / "shaders" / file,
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
    clear_program_ = make_probe_compute("probe_capture.slang", "clear_main");
    collapse_program_ = make_probe_compute("probe_capture.slang", "collapse_main");
    relight_program_ = make_probe_compute("probe_relight.slang", "relight_main");

    // Cube-face G-buffer raster: task/mesh/fragment MRT (albedo + normal-dist) + depth. Modelled on
    // the meshlet_shadow pipeline; two RGBA16F colour targets, D32 depth (reverse-Z GREATER), and the
    // 6-face multiview view mask.
    capture_program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "probe_capture_raster.slang",
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
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
            }
            p.pipeline = builder
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_multisampling()
                .enable_depth_stencil(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                .disable_color_blending()
                .set_color_formats({ VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT })
                .set_view_mask(kProbeCubeViewMask)   // 6-face multiview: SV_ViewID picks the face vp
                .build_mesh_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // Debug-sphere graphics pipeline (instanced, procedural sphere; MSAA + depth-test, no blend).
    debug_program_ = ctx.shader_registry.create(
        ctx.resources_path / "shaders" / "probe_debug.slang",
        [layout, scene_samples](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
            ::string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::GRAPHICS);
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
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT)
                .set_multisampling(scene_samples)
                .enable_depth_stencil(true, true)   // reverse-Z: GREATER (matches scene pipelines)
                .enable_color_blending()
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // The capture table's host copy. The TABLE is a graph buffer the app declares; this is the
    // staging the one-shot gi.clear pass copies from, so the upload is a declared transfer with a
    // derived edge into the capture task shader rather than a construction-time submit.
    const std::vector<glm::uvec2> mdraw = capture_table(
        scene_->draw_info_mapped_ != nullptr
            ? std::span<const GpuDrawInfo>(scene_->draw_info_mapped_, scene_->active_draw_count_)
            : std::span<const GpuDrawInfo>{});
    probe_total_meshlets_ = static_cast<uint32_t>(mdraw.size());
    table_bytes_ = sizeof(glm::uvec2) * std::max<size_t>(mdraw.size(), 1);
    table_staging_ = allocator_->create_resource(::string::gpu::buffer_info{
        .size = table_bytes_,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    });
    auto* dst = static_cast<glm::uvec2*>(
        allocator_->get_buffer(table_staging_).allocation_info.pMappedData);
    if (mdraw.empty()) dst[0] = glm::uvec2{ 0, 0 };
    else std::copy(mdraw.begin(), mdraw.end(), dst);
    STRING_LOG_INFO("[gi] capture dispatch domain: {} coarse-LOD meshlets over {} draws "
                    "(per-probe task groups: {})", probe_total_meshlets_,
                    scene_->active_draw_count_, (probe_total_meshlets_ + 31u) / 32u);

    scene_->gi = this;
}

probe_gi_component::~probe_gi_component()
{
    if (scene_ != nullptr) scene_->gi = nullptr;
    const auto kill = [this](::string::gpu::shader_program* prog) {
        if (prog == nullptr) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_->get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_->get_device(), p.pipeline_layout, nullptr);
    };
    kill(clear_program_);
    kill(collapse_program_);
    kill(relight_program_);
    kill(capture_program_);
    kill(debug_program_);
    allocator_->destroy_resource(table_staging_);
}

// --- authoring -----------------------------------------------------------------------------------

void probe_gi_component::declare(::string::frame_graph& fg, const resources& res,
                                 ::string::gpu::buffer sky_sh, ::string::gpu::buffer scene_data,
                                 std::span<const ::string::gpu::image> cascades,
                                 ::string::gpu::image scene_color, ::string::gpu::image scene_depth)
{
    res_ = res;
    sky_sh_ = sky_sh;
    scene_data_ = scene_data;

    // The cube G-buffer's render target is the 6-layer slice, not the cube's default CUBE view: the
    // multiview pass writes all six layers at once. Declaring the slice is also what makes the
    // derived acquire/release cover exactly the six layers the old hand-rolled transitions named.
    const auto all_faces = [](::string::gpu::image img) {
        return ::string::gpu::image_view{ img, 0, 1, 0, 6 };
    };

    // 1) One-shot init. The four atlases and the two per-probe buffers get their starting contents,
    //    and the capture table is uploaded. The irradiance atlas is ZEROED because relight is
    //    amortized behind capture completion: shading and the debug spheres can legally sample texels
    //    before their first relight, and they must read black rather than garbage.
    //
    //    The old UNDEFINED -> GENERAL prologue (:407) is gone: a first write discards by definition,
    //    which is what the tracker already does. The clear -> collapse flush (:455) is gone too: it
    //    is the read edge between this pass's writes and the collapse's, and both are declared.
    fg.pass("gi.clear")
      .writes(res_.meshlet_table, ::string::access::transfer_write)
      .writes(res_.active, ::string::access::transfer_write)
      .writes(res_.offset, ::string::access::transfer_write)
      .writes(res_.irradiance, ::string::access::transfer_write)
      .writes(res_.capture_gbuf)
      .writes(res_.capture_albedo)
      .writes(res_.visibility)
      .toggle([this] { return probe_volume_.valid && !init_done_ && cv_gi_enabled().get(); })
      .compute([this](::string::pass_context& ctx) { record_clear(ctx); });

    // 2) The per-probe bake, unrolled over the frame's capture slots. Each slot is a raster pass
    //    (all six cube faces in ONE multiview draw) followed by the compute that collapses that cube
    //    into the probe's atlas tile. The cube trio is destructively reused between slots, so the
    //    slot-to-slot ordering is pure WAR/WAW over one resource — declared, therefore derived
    //    (:498, :507, :587 and :631 all delete).
    for (uint32_t slot = 0; slot < kProbesPerFrame; ++slot)
    {
        ::string::pass_spec capture = fg.pass("gi.capture." + std::to_string(slot));
        capture
          .color(all_faces(res_.cube_albedo))
          .color(all_faces(res_.cube_nd))
          .depth(all_faces(res_.cube_depth))
          // The task shader resolves each flat dispatch index to {draw, meshlet} through the table,
          // and both task and mesh add this probe's relocation offset. Storage reads at task/mesh are
          // the one case an access genuinely cannot imply its stage.
          .reads(res_.meshlet_table, ::string::access::storage_read,
                 VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(res_.offset, ::string::access::storage_read,
                 VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
        // The registry's content heaps (assets::gpu_view, latched into the scene tables): the
        // capture's meshlet path pulls geometry exactly like the main phases.
        for (const ::string::gpu::buffer& h :
             { scene_->vertex_buffer_, scene_->meshlet_buffer_, scene_->meshlet_vertices_,
               scene_->meshlet_triangles_ })
        {
            if (!h.valid()) continue;
            capture.reads(h, ::string::access::storage_read,
                          VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT
                              | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
        }
        capture
          .toggle([this, slot] { return capture_slot_runs(slot); })
          .raster([this, slot](::string::pass_context& ctx) { record_capture(ctx, slot); });

        fg.pass("gi.collapse." + std::to_string(slot))
          // SamplerCube reads of the whole cube: there is no per-face sequencing to declare.
          .reads(res_.cube_albedo)
          .reads(res_.cube_nd)
          // The per-probe unit is a TILE inside the atlas, which is not a Vulkan subresource, so
          // these are honest whole-image writes and no barrier changes because of it.
          .writes(res_.capture_gbuf)
          .writes(res_.capture_albedo)
          .writes(res_.visibility)
          .writes(res_.active)
          .writes(res_.offset)
          .toggle([this, slot] { return capture_slot_runs(slot); })
          .compute([this, slot](::string::pass_context& ctx) { record_collapse(ctx, slot); });
    }

    // 3) The amortized relight.
    //
    //    probe_irradiance is TEMPORAL and stays ONE logical image: the dispatch reads the previous
    //    contents (hysteresis EMA + a neighbour-bounce term over probes last written on an EARLIER
    //    frame) and writes them back, for the 128 probes in this frame's round-robin slice. That is a
    //    read-modify-write — ONE declaration, `.read_writes()` (brief 11's verb, restored by brief
    //    21 D3): combined read|write scope at GENERAL, no declaration-order dependence. The two
    //    descriptors the shader needs (Sampler2D for the filtered bounce, RWTexture2D for the
    //    store) both resolve through the access-qualified ctx.slot(); the graph binds both types
    //    for a read_writes use at compile.
    //
    //    Three barriers die here. :673 made the IBL's SH write visible to this compute read, and the
    //    IBL chain is declared now, so the RAW derives. :689 was the cross-frame WAR against last
    //    frame's shading reads, which derives now that tracked state carries across the frame
    //    boundary. :739 bundled two edges — this write into the lit fragments (the consumer declares
    //    its read) and the WAR against the shadow passes that WRITE the cascades this relight sampled
    //    (declared below, and ordered by this component being authored before shadow_maps).
    ::string::pass_spec relight = fg.pass("gi.relight");
    relight.reads(res_.capture_gbuf)
           .reads(res_.capture_albedo)
           .reads(res_.visibility)
           .read_writes(res_.irradiance)
           .reads(sky_sh_)
           .reads(scene_data_)
           .reads(res_.active)
           .reads(res_.offset);
    // The 1-tap sun shadow samples the CSM maps through SceneData's bindless slots, so nothing here
    // resolves them — the read is declared because it is what the WAR against the shadow passes that
    // rewrite the cascades LATER this frame derives from. It was the second half of the deleted :739.
    for (const ::string::gpu::image& cascade : cascades) relight.reads(cascade);
    relight.toggle([this] { return relight_runs_; })
           .compute([this](::string::pass_context& ctx) { record_relight(ctx); });

    // 4) The instanced probe-debug spheres, into the scene attachments (depth-tested, like the debug
    //    line pass). One instance per probe; the sphere is procedural.
    fg.pass("gi.debug")
      .color(scene_color)
      .depth(scene_depth)
      .reads(res_.irradiance)
      .reads(res_.visibility)
      // The vertex shader reads both buffers (inactive probes are hidden, relocated ones moved).
      .reads(res_.active, ::string::access::storage_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT)
      .reads(res_.offset, ::string::access::storage_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT)
      .toggle([this] { return probe_volume_.valid && debug_mode_ != 0; })
      .raster([this](::string::pass_context& ctx) { record_debug(ctx); });
}

// --- per-frame CPU decisions ---------------------------------------------------------------------

void probe_gi_component::tick()
{
    const bool enabled = cv_gi_enabled().get();
    debug_mode_ = enabled ? static_cast<uint32_t>(std::max(0, cv_gi_probe_debug().get())) : 0u;

    if (scene_ == nullptr || !probe_volume_.valid || !enabled || scene_->ibl == nullptr)
    {
        capture_count_ = 0;
        relight_runs_ = false;
        return;
    }

    // A sun move (or first light) ARMS a full set of converge passes: each amortized full pass
    // propagates the bounce one step further and settles the hysteresis EMA; when the passes run out,
    // relight goes idle until the next trigger (~0 static-sun cost). Relight depends on the sky-SH
    // (miss/fallback source), so the IBL must have primed first.
    constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg) — matches the IBL trigger
    const float align = glm::dot(glm::normalize(scene_->sun_dir_), relit_sun_dir_);
    if (scene_->ibl->primed() && (!primed_ || cv_ibl_every_frame().get() || align < kSunDeltaCos))
    {
        relight_passes_left_ = kRelightConvergePasses;
        relight_pending_ = true;
    }

    // --- capture amortization. TWO bake rounds (RTXGI-style relocation consistency): round 1 rasters
    // each probe from its GRID position and the collapse derives the relocation offset +
    // classification from that view. Round 2 re-rasters from the RELOCATED position and re-collapses,
    // so the visibility/hit distances stored in the atlases are measured from the SAME position every
    // consumer uses via probe_world().
    const uint32_t total = probe_volume_.total();
    const uint32_t total_work = total * 2;
    const bool capture_ready = clear_program_ != nullptr && capture_program_ != nullptr
                            && collapse_program_ != nullptr && scene_->draw_info_mapped_ != nullptr;
    capture_base_ = capture_cursor_;
    // The bake may start in the SAME frame the init pass runs: gi.clear is authored first and the
    // edges into it are real (the table the capture reads, the atlases the collapse writes), so the
    // graph orders it ahead without this needing to know.
    capture_count_ = (!captured_ && capture_ready ? std::min(kProbesPerFrame, total_work - capture_cursor_) : 0u);
    capture_cursor_ += capture_count_;
    if (capture_cursor_ >= total_work && !captured_ && capture_ready)
    {
        captured_ = true;
        STRING_LOG_INFO("[gi] probe capture complete: {} probes x 2 rounds (relocate + re-capture), "
                        "{:.1f} ms CPU-record over frames", total, capture_ms_);
    }

    // --- relight amortization. Relight starts only after the capture COMPLETES: partially-captured
    // probes would relight from the cleared (all-sky-miss) capture and read as outdoor probes until
    // their capture landed. It also guarantees the CSM shadow maps this slot samples have been
    // rendered at least once.
    relight_runs_ = captured_ && relight_pending_ && relight_program_ != nullptr
                 && scene_->ibl->primed() && sky_sh_.valid() && scene_data_.valid();
    if (!relight_runs_) return;

    relight_base_ = relight_cursor_;
    relight_count_ = std::min(kRelightProbesPerFrame, total - relight_base_);
    // The dispatch pushes the primed state as it was BEFORE this frame's slice: the wrapping slice
    // itself must still run un-hysteresed if it is the first full pass.
    relight_first_ = !primed_;
    relight_cursor_ = relight_base_ + relight_count_;
    if (relight_cursor_ >= total)
    {
        relight_cursor_ = 0;
        primed_ = true;
        relit_sun_dir_ = glm::normalize(scene_->sun_dir_);
        if (relight_passes_left_ > 0) --relight_passes_left_;
        relight_pending_ = relight_passes_left_ > 0;
    }
}

// --- pass bodies ---------------------------------------------------------------------------------

// One-time init: default the activation + relocation buffers, upload the capture table, zero the
// irradiance atlas, and prime the capture/visibility atlases over ALL probes (clear_main).
void probe_gi_component::record_clear(::string::pass_context& ctx)
{
    ::string::gpu::command_recorder& rec = ctx.rec;
    const uint32_t total = probe_volume_.total();

    const VkBufferCopy region = { 0, 0, table_bytes_ };
    rec.copy_buffer(allocator_->get_buffer(table_staging_).buffer,
                    allocator_->get_buffer(ctx.id(res_.meshlet_table)).buffer, 1, &region);
    rec.fill_buffer(allocator_->get_buffer(ctx.id(res_.active)).buffer, 0, VK_WHOLE_SIZE, 1u);   // default active
    rec.fill_buffer(allocator_->get_buffer(ctx.id(res_.offset)).buffer, 0, VK_WHOLE_SIZE, 0u);   // zero relocation

    const VkClearColorValue zero{ .float32 = { 0, 0, 0, 0 } };
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    rec.clear_color_image(ctx.vk_image(res_.irradiance.whole()),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, zero, 1, &range);

    ProbeCapturePush cp{};
    cp.probe_base = 0;
    cp.probe_count = total;
    cp.counts_x = probe_volume_.counts.x;
    cp.counts_y = probe_volume_.counts.y;
    cp.counts_z = probe_volume_.counts.z;
    cp.cap_gbuf_slot = ctx.slot(res_.capture_gbuf);
    cp.cap_albedo_slot = ctx.slot(res_.capture_albedo);
    cp.vis_slot = ctx.slot(res_.visibility);
    cp.far_distance = glm::length(scene_->scene_aabb_max_ - scene_->scene_aabb_min_);

    const ::string::gpu::pipeline& p = clear_program_->current();
    rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1,
                             &descriptor_set_, 0, nullptr);
    rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeCapturePush), &cp);
    rec.dispatch((kProbeVisStride + 7) / 8, (kProbeVisStride + 7) / 8, total);

    init_done_ = true;
}

// One probe's cube G-buffer: all 6 faces in ONE multiview draw (viewMask 0x3F) over the 6-layer
// attachment slices; the mesh shader picks the per-face view-projection by SV_ViewID. The dispatch is
// a SINGLE flat draw over the coarse-LOD meshlets — the task shader resolves each flat index to
// {draw, meshlet} via the load-time table, GPU frustum-culls against all six faces, and amplifies
// only survivors. No CPU per-draw loop, no CPU culling, and no vkCmdBeginRendering: the pass declared
// its attachments, so the framework opens the render pass and derives the clears from this being the
// cube's first write.
void probe_gi_component::record_capture(::string::pass_context& ctx, uint32_t slot)
{
    const auto t0 = std::chrono::steady_clock::now();
    ::string::gpu::command_recorder& rec = ctx.rec;
    const uint32_t total = probe_volume_.total();
    const uint32_t probe = capture_work(slot) % total;
    const glm::uvec3 counts = probe_volume_.counts;
    const glm::uvec3 c{ probe % counts.x, (probe / counts.x) % counts.y,
                        probe / (counts.x * counts.y) };
    const glm::vec3 probe_pos = probe_volume_.origin + glm::vec3(c) * probe_volume_.spacing;

    const ::string::gpu::pipeline& p = capture_program_->current();
    rec.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline_layout, 0, 1,
                             &descriptor_set_, 0, nullptr);

    // The geometry heaps are the scene's, not this pass's: they are long-lived allocator resources
    // every meshlet path pushes by address (the same shape shadow_maps uses).
    ProbeRasterPush push{};
    push.vertices = ctx.address(scene_->vertex_buffer_);
    push.meshlets = ctx.address(scene_->meshlet_buffer_);
    push.mverts = ctx.address(scene_->meshlet_vertices_);
    push.mtris = ctx.address(scene_->meshlet_triangles_);
    push.draws = allocator_->get_buffer(scene_->draw_info_buffer_).device_address;
    push.mdraw = ctx.address(res_.meshlet_table);
    push.probe_pos = probe_pos;
    push.meshlet_count = probe_total_meshlets_;
    push.cull_far = probe_cull_far_;
    push.probe_index = probe;
    push.offsets = ctx.address(res_.offset);
    rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeRasterPush), &push);
    rec.draw_mesh_tasks((probe_total_meshlets_ + 31u) / 32u, 1, 1);   // ONE flat GPU-driven dispatch

    capture_ms_ += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// Collapse this probe's cube into its octahedral atlas tiles + classify + relocate. One workgroup
// (18x18) per probe.
void probe_gi_component::record_collapse(::string::pass_context& ctx, uint32_t slot)
{
    ::string::gpu::command_recorder& rec = ctx.rec;
    const uint32_t total = probe_volume_.total();
    const uint32_t work = capture_work(slot);
    const uint32_t probe = work % total;
    const uint32_t round = work / total;
    const glm::uvec3 counts = probe_volume_.counts;
    const glm::uvec3 c{ probe % counts.x, (probe / counts.x) % counts.y,
                        probe / (counts.x * counts.y) };

    ProbeCapturePush cp{};
    cp.counts_x = counts.x;
    cp.counts_y = counts.y;
    cp.counts_z = counts.z;
    cp.cap_gbuf_slot = ctx.slot(res_.capture_gbuf);
    cp.cap_albedo_slot = ctx.slot(res_.capture_albedo);
    cp.vis_slot = ctx.slot(res_.visibility);
    cp.far_distance = glm::length(scene_->scene_aabb_max_ - scene_->scene_aabb_min_);
    cp.cube_albedo_slot = ctx.slot(res_.cube_albedo);
    cp.cube_nd_slot = ctx.slot(res_.cube_nd);
    cp.probe_pos = probe_volume_.origin + glm::vec3(c) * probe_volume_.spacing;
    cp.probe_index = probe;
    cp.round2 = round;
    cp.active = ctx.address(res_.active);
    cp.spacing = probe_volume_.spacing;
    cp.offset = ctx.address(res_.offset);

    const ::string::gpu::pipeline& p = collapse_program_->current();
    rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1,
                             &descriptor_set_, 0, nullptr);
    rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeCapturePush), &cp);
    rec.dispatch(1, 1, 1);   // one workgroup (18x18) per probe
}

// M2 dynamic relight -> irradiance atlas: capture-driven radiance (miss -> live sky; hit -> albedo x
// (1-tap-CSM-shadowed sun + bounce from the previous atlas)) cosine-convolved per octa texel with
// hysteresis, over this frame's round-robin slice.
void probe_gi_component::record_relight(::string::pass_context& ctx)
{
    ::string::gpu::command_recorder& rec = ctx.rec;

    ProbeRelightPush push{};
    push.origin_spacing = probe_origin_spacing(probe_volume_);
    push.spacing = glm::vec4(probe_volume_.spacing, 0.0f);
    push.counts = glm::uvec4(probe_volume_.counts, probe_volume_.total());
    push.sun_dir = glm::vec4(glm::normalize(scene_->sun_dir_), scene_->sun_intensity_);
    push.sun_color = glm::vec4(scene_->sun_color_,
        relight_first_ ? 0.0f : std::clamp(cv_gi_hysteresis().get(), 0.0f, 0.99f));
    push.sky_zenith = glm::vec4(scene_->sky_zenith_, 0.0f);
    push.sky_ground = glm::vec4(scene_->sky_ground_, 0.0f);
    push.cap_gbuf_slot = ctx.slot(res_.capture_gbuf);
    push.cap_albedo_slot = ctx.slot(res_.capture_albedo);
    push.irrad_prev_slot = ctx.slot(res_.irradiance, ::string::access::sampled_read);
    // Two descriptor types for ONE image in one dispatch: the hysteresis samples the previous
    // irradiance and storage-writes the new one. The access disambiguates, which is honest — it is
    // what the pass already declared — where the old .mip(0) slice was a trick to force two slots.
    push.irrad_dst_slot = ctx.slot(res_.irradiance, ::string::access::storage_image_write);
    push.vis_slot = ctx.slot(res_.visibility);
    push.first_frame = relight_first_ ? 1u : 0u;
    push.probe_base = relight_base_;
    push.probe_count = relight_count_;
    push.sh = ctx.address(sky_sh_);
    push.active = ctx.address(res_.active);
    push.offsets = ctx.address(res_.offset);
    push.scene = ctx.address(scene_data_);

    const ::string::gpu::pipeline& p = relight_program_->current();
    rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1,
                             &descriptor_set_, 0, nullptr);
    rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeRelightPush), &push);
    // One workgroup (10x10 threads) per probe in this frame's slice; z = probe.
    rec.dispatch(1, 1, relight_count_);
}

void probe_gi_component::record_debug(::string::pass_context& ctx)
{
    ::string::gpu::command_recorder& rec = ctx.rec;
    const ::string::gpu::pipeline& p = debug_program_->current();

    ProbeDebugPush push{};
    push.view_proj = scene_->camera_.view_proj();
    const float min_sp = std::min(probe_volume_.spacing.x,
                                  std::min(probe_volume_.spacing.y, probe_volume_.spacing.z));
    push.origin_spacing = glm::vec4(probe_volume_.origin, min_sp * 0.15f);   // sphere radius
    push.spacing = glm::vec4(probe_volume_.spacing, 0.0f);
    push.counts = glm::uvec4(probe_volume_.counts, debug_mode_);   // 1 grey, 2 irradiance, 3 vis
    push.camera_pos = glm::vec4(scene_->camera_.position(), composite_pass::exposure_scale());
    push.irrad_slot = ctx.slot(res_.irradiance);
    push.vis_slot = ctx.slot(res_.visibility);
    push.active = ctx.address(res_.active);
    push.offset = ctx.address(res_.offset);

    rec.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline_layout, 0, 1,
                             &descriptor_set_, 0, nullptr);
    rec.push_constants(p.pipeline_layout, p.push_constants.stageFlags, 0, sizeof(ProbeDebugPush), &push);
    // rings*sectors*6 verts per sphere (kRings=8, kSectors=12 -> 576), one instance per probe.
    rec.draw(8u * 12u * 6u, probe_volume_.total(), 0, 0);
}

// --- shading-side publication --------------------------------------------------------------------

// The shading-side gate: 0 under r.gi 0 (the pixel-parity lever — the shader then takes the pre-09b
// sky-SH path bit-identically), under r.furnace (the furnace validates the BRDF against the analytic
// environment, same rule as GTAO), and until the capture + FIRST FULL relight pass complete (the
// atlas is zero-cleared before that — sampling it would darken instead of falling back).
void probe_gi_component::fill_scene_data(SceneData& scene, ::string::pass_context& ctx) const
{
    const bool on = probe_volume_.valid && cv_gi_enabled().get() && !scene_->furnace_
                 && captured_ && primed_;
    scene.probe_origin = probe_volume_.origin;
    scene.probe_spacing = probe_volume_.spacing;
    scene.probe_counts = probe_volume_.counts;
    scene.probe_irrad_slot = ctx.slot(res_.irradiance);
    scene.probe_vis_slot = ctx.slot(res_.visibility);
    // probe_gi: 0 off, 1 normal probe-GI shading, 2 = GI-debug isolate view (indirect diffuse
    // x albedo only) — the reference-free-scene judging tool (r.gi.debug 1).
    scene.probe_gi = on ? (cv_gi_debug().get() != 0 ? 2u : 1u) : 0u;
    scene.probe_offsets = ctx.address(res_.offset);
    scene.probe_active = ctx.address(res_.active);
    scene.probe_occluded_floor = std::clamp(cv_gi_occluded_floor().get(), 0.0f, 1.0f);
}

}  // namespace string::render
