// Brief 09b probe GI (relightable irradiance volume) — split out of geometry_pass.cpp (brief 11
// modularization). These remain GeometryPass member functions: the probe stage is woven into the
// geometry core (it rasters the meshlets, reads the SceneData/CSM), so this is a cohesive
// translation-unit split, not an object extraction. Phase 2 (graph passes) does the true decoupling.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
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

#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace String;

// --- Brief 09b: probe volume ---------------------------------------------------------------------

// Fit a uniform probe grid to the scene AABB using the spacing CVar. Padded half a cell so the
// volume boundary probes sit just outside the geometry (edge interpolation stays valid). Counts
// clamped per-axis and by total (atlas VRAM / relight cost guard).
void GeometryPass::fit_probe_volume()
{
    const glm::vec3 ext = scene_aabb_max_ - scene_aabb_min_;
    if (ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f)
    {
        probe_volume_.valid = false;
        return;
    }
    float spacing = cv_gi_spacing().get();
    if (spacing <= 0.0f)
    {
        // Auto: aim for ~16 probes along the longest axis.
        const float longest = std::max(ext.x, std::max(ext.y, ext.z));
        spacing = std::max(longest / 16.0f, 0.25f);
    }
    // Anisotropic spacing (main-agent tip 6): architectural scenes vary less vertically than
    // horizontally, and a coarser Y is a cheap probe-count win — while still keeping >=2 layers in
    // any room. Y spacing is 1.5x the horizontal spacing.
    glm::vec3 sp{ spacing, spacing * 1.5f, spacing };

    // Grid FIXED-spacing, fit to a TRUE half-cell inset of the AABB (main-agent tip 2): geometry
    // sits at axis-aligned positions, so a grid flush to the AABB plants probes exactly in
    // floors/walls/column axes. The outermost probe layer on every axis lands within half a cell
    // INSIDE the AABB (never outside it): with the centred origin below, `ceil(e/s)` probes span
    // [min + up to 0.5s, max - up to 0.5s]. (The previous `ceil(e/s) + 1` planted an extra layer a
    // full half-cell OUTSIDE each face — a wasted probe layer below the floor and above the roofline,
    // which is exactly the stranded-probe artifact this fixes.) Trilinear clamps at the volume edge,
    // so boundary surfaces (floor, outer walls) sample the nearest in-volume layer.
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
    // the leftover `ext - grid_span` (in [0, sp]) splits evenly and both end layers sit within half
    // a cell INSIDE their faces — symmetric, hugging neither min nor max.
    const glm::vec3 grid_span = glm::vec3(counts - glm::uvec3(1u)) * sp;
    probe_volume_.origin = scene_aabb_min_ + (ext - grid_span) * 0.5f;
    probe_volume_.spacing = sp;
    probe_volume_.counts = counts;
    probe_volume_.valid = true;

    // ANALYTIC capture distance-cull radius (derived, not tuned). The visibility atlas only ever
    // answers Chebyshev queries from shading points inside a probe's ADJACENT cells (the 8-probe
    // trilinear samples the enclosing cell's corners), so the largest distance that must be captured
    // accurately is exactly:
    //     r_vis = |spacing|            worst-case shading point at the opposite cell corner
    //           + 0.5 * max(spacing)   relocation can move the probe up to half a cell
    //           + 0.75 * min(spacing)  the DDGI normal/view sampling bias applied at shading
    // Geometry beyond r_vis can read as "far" without changing ANY visibility result, and the radius
    // now scales with the grid: tighter spacing -> tighter radius -> cheaper bake, automatically.
    // Radiance caveat (documented trade-off): the M2 relight treats first-hits beyond the radius as
    // open sky. Acceptable for Sponza-class scenes (interior ceilings sit well within r_vis of their
    // probes; the tall central atrium genuinely is open sky) — flagged in the brief's running log as
    // the term that stops the radius going tighter than r_vis.
    const float diag = glm::length(ext);
    const float cell_diag = glm::length(sp);
    const float r_vis = cell_diag + 0.5f * std::max(sp.x, std::max(sp.y, sp.z))
                      + 0.75f * std::min(sp.x, std::min(sp.y, sp.z));
    probe_cull_far_ = std::min(diag, r_vis);

    const uint32_t total = probe_volume_.total();
    const glm::uvec2 vtiles = probe_volume_.tile_grid();
    const uint32_t irrad_w = vtiles.x * kProbeIrradStride, irrad_h = vtiles.y * kProbeIrradStride;
    const uint32_t vis_w = vtiles.x * kProbeVisStride, vis_h = vtiles.y * kProbeVisStride;
    // Atlas memory: irradiance RGBA16F, vis RG16F(as RGBA16F), 2x capture RGBA16F.
    const double mb = (double(irrad_w) * irrad_h * 8.0             // irradiance RGBA16F
                       + double(vis_w) * vis_h * 8.0               // visibility
                       + double(vis_w) * vis_h * 8.0 * 2.0)        // capture gbuf + albedo
                      / (1024.0 * 1024.0);
    STRING_LOG_INFO("[gi] probe grid {}x{}x{} = {} probes, spacing ({:.2f},{:.2f},{:.2f}) m, "
                    "irrad atlas {}x{}, vis atlas {}x{}, ~{:.2f} MB",
                    counts.x, counts.y, counts.z, total, sp.x, sp.y, sp.z,
                    irrad_w, irrad_h, vis_w, vis_h, mb);
}

void GeometryPass::create_probe_resources(engine_context& context)
{
    fit_probe_volume();
    if (!probe_volume_.valid) return;

    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };
    if (vkCreateSampler(device_.get_device(), &sampler_info, nullptr, &probe_sampler_) != VK_SUCCESS)
        throw std::runtime_error("GeometryPass: failed to create probe sampler");

    const glm::uvec2 vtiles = probe_volume_.tile_grid();
    const auto make_atlas = [&](uint32_t stride, uint32_t& sample_slot, uint32_t& storage_slot) {
        const ::string::gpu::resource_id id = allocator_.create_resource(::string::gpu::image_info{
            .extent = { vtiles.x * stride, vtiles.y * stride, 1 },
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            // TRANSFER_DST: the capture init vkCmdClearColorImage-zeroes the irradiance atlas
            // (relight is amortized, so shading/debug can read texels before their first relight).
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        const ::string::gpu::allocated_image& img = allocator_.get_image(id);
        descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);
        sample_slot = descriptor_table_.get_binding_slot(id, ::string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(sample_slot, img.view, probe_sampler_);
        storage_slot = descriptor_table_.bind_storage_view(img.view);
        return id;
    };
    probe_irrad_ = make_atlas(kProbeIrradStride, probe_irrad_sample_slot_, probe_irrad_storage_slot_);
    probe_cap_gbuf_ = make_atlas(kProbeVisStride, probe_cap_gbuf_sample_slot_, probe_cap_gbuf_storage_slot_);
    probe_cap_albedo_ = make_atlas(kProbeVisStride, probe_cap_albedo_sample_slot_, probe_cap_albedo_storage_slot_);
    probe_vis_ = make_atlas(kProbeVisStride, probe_vis_sample_slot_, probe_vis_storage_slot_);

    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    const auto make_probe_compute = [&](const char* file, const char* entry) {
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
    probe_clear_program_ = make_probe_compute("probe_capture.slang", "clear_main");
    probe_collapse_program_ = make_probe_compute("probe_capture.slang", "collapse_main");
    probe_relight_program_ = make_probe_compute("probe_relight.slang", "relight_main");

    // Cube G-buffer (albedo + normal/dist + depth), reused across probes within a frame. All 6 faces
    // render in ONE multiview pass, so each image gets a single 6-layer 2D_ARRAY view (the render
    // target, viewMask=0x3F) + a SamplerCube read slot for collapse.
    const auto make_gbuf_cube = [&](VkFormat fmt, VkImageAspectFlags aspect, VkImageUsageFlags usage,
                                    VkImageView& array_view, uint32_t* sample_slot) {
        const ::string::gpu::resource_id id = allocator_.create_resource(::string::gpu::image_info{
            .extent = { kProbeCubeFace, kProbeCubeFace, 1 },
            .format = fmt,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .aspect_flags = aspect,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
            .cube = true,
        });
        const ::string::gpu::allocated_image& img = allocator_.get_image(id);
        if (sample_slot != nullptr)   // SamplerCube read slot (albedo / normal-dist only; not depth)
        {
            descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);
            *sample_slot = descriptor_table_.get_binding_slot(id, ::string::gpu::descriptor_type::TEXTURE);
            descriptor_table_.update_texture(*sample_slot, img.view, probe_sampler_);
        }
        const VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = img.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format = fmt,
            .subresourceRange = { aspect, 0, 1, 0, 6 },   // all 6 cube layers (multiview target)
        };
        if (vkCreateImageView(device_.get_device(), &vi, nullptr, &array_view) != VK_SUCCESS)
            throw std::runtime_error("GeometryPass: failed to create probe cube array view");
        return id;
    };
    probe_cube_albedo_ = make_gbuf_cube(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        probe_cube_albedo_array_view_, &probe_cube_albedo_sample_slot_);
    probe_cube_nd_ = make_gbuf_cube(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        probe_cube_nd_array_view_, &probe_cube_nd_sample_slot_);
    probe_cube_depth_ = make_gbuf_cube(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, probe_cube_depth_array_view_, nullptr);

    // Per-probe activation state (classification output): 1 = active, 0 = inside geometry.
    probe_active_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = std::max<VkDeviceSize>(sizeof(uint32_t) * probe_volume_.total(), sizeof(uint32_t)),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Per-probe relocation offset (RTXGI): float4 per probe (xyz world offset). Written by the collapse;
    // consumed via probe_common probe_world (relight bounce / M3 shading) + the debug spheres.
    probe_offset_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = std::max<VkDeviceSize>(sizeof(glm::vec4) * probe_volume_.total(), sizeof(glm::vec4)),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // Flat-capture -> {draw index, global meshlet id} table, built ONCE from the meshlet model so the
    // capture task shader resolves a flat dispatch index to its draw + real global meshlet id with no
    // CPU per-draw loop. Entry layout: {uint draw, uint meshlet_id} per flat slot.
    //
    // The capture uses each draw's COARSEST LOD, not LOD0: the cube faces are 32x32 px, so full-detail
    // geometry is pure waste — the bake cost is dominated by (meshlets x 64 vertex transforms) per
    // probe, and the coarse LOD cuts the meshlet count ~an order of magnitude (Sponza: 122k LOD0
    // meshlets over 450 draws). GI capture on proxy/low-LOD geometry is the shipped-engine standard;
    // the slight surface shift is far below the probe grid's spatial resolution.
    {
        std::vector<glm::uvec2> mdraw;   // (draw index, global meshlet id)
        for (uint32_t d = 0; d < meshlet_model_.draws.size(); ++d)
        {
            const GpuDrawInfo& di = meshlet_model_.draws[d];
            if (di.lod_count == 0) continue;
            const uint32_t coarse = di.lod_count - 1;
            const uint32_t off = di.lods[coarse].meshlet_offset;
            const uint32_t cnt = di.lods[coarse].meshlet_count;
            for (uint32_t m = 0; m < cnt; ++m) mdraw.push_back({ d, off + m });
        }
        probe_total_lod0_meshlets_ = static_cast<uint32_t>(mdraw.size());
        if (mdraw.empty()) mdraw.push_back({ 0, 0 });
        probe_meshlet_draw_ = allocator_.create_resource(::string::gpu::buffer_info{
            .size = sizeof(glm::uvec2) * mdraw.size(),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(mdraw.data(), sizeof(glm::uvec2) * mdraw.size(), probe_meshlet_draw_);
        STRING_LOG_INFO("[gi] capture dispatch domain: {} coarse-LOD meshlets over {} draws "
                        "(per-probe task groups: {})", probe_total_lod0_meshlets_,
                        meshlet_model_.draws.size(), (probe_total_lod0_meshlets_ + 31u) / 32u);
    }

    // M1 cube-face G-buffer raster: task/mesh/fragment MRT (albedo + normal-dist) + depth. Modelled
    // on the meshlet_shadow pipeline; two RGBA16F color targets, D32 depth (reverse-Z GREATER).
    probe_capture_raster_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "probe_capture_raster.slang",
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
    const VkSampleCountFlagBits scene_samples = context.sample_count;
    probe_debug_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "probe_debug.slang",
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
}

// Fill the ProbeRelightPush grid fields from probe_volume_ (shared by relight + debug).
namespace
{
glm::vec4 probe_origin_spacing(const ProbeVolume& v) { return glm::vec4(v.origin, v.spacing.x); }
}  // namespace

namespace
{
// The M1 collapse push mirrors probe_capture.slang's Push exactly (std430; offsets verified against
// %Push_std430 OpMemberDecorate: probe_pos@48, probe_index@60, active ptr@64).
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

// NOTE: the per-face cube-face basis + view-projection now live IN probe_capture_raster.slang
// (face_view_proj / kFaceF/R/U), built from probe_pos so the push stays tiny (6 mat4 would blow the
// 256B budget). The convention still mirrors ibl.slang face_dir() so the collapse SamplerCube reads
// agree with the raster writes.
}  // namespace

// Static capture, amortized over frames (brief 09b M1). First call: init all atlases (clear_main) +
// zero the activation buffer + move cube G-buffers to their layouts. Every call: capture the next K
// probes — for each, rasterize the static scene into a 6-face cube G-buffer (albedo + world normal +
// linear distance) then collapse it into that probe's octahedral capture + visibility atlases, and
// classify it (inside-geometry probes -> INACTIVE). probe_captured_ latches once the cursor wraps;
// the atlases are STATIC thereafter (only relight re-runs).
void GeometryPass::record_probe_capture(::string::gpu::command_recorder& recorder)
{
    if (!probe_volume_.valid || probe_clear_program_ == nullptr || probe_capture_raster_program_ == nullptr
        || probe_collapse_program_ == nullptr || draw_info_mapped_ == nullptr)
        return;
    VkCommandBuffer cb = recorder.vk();   // probe capture is raster+compute+barrier raw Vulkan (vk() escape)
    VkDescriptorSet set = descriptor_table_.get_set();
    const uint32_t total = probe_volume_.total();
    const float far_distance = glm::length(scene_aabb_max_ - scene_aabb_min_);
    const auto t0 = std::chrono::steady_clock::now();

    const auto counts = probe_volume_.counts;

    // --- One-time init: atlas layouts + clear_main over all probes + zero activation ---------------
    if (!probe_layouts_initialized_)
    {
        for (::string::gpu::resource_id id : { probe_irrad_, probe_cap_gbuf_, probe_cap_albedo_, probe_vis_ })
        {
            const ::string::gpu::allocated_image& img = allocator_.get_image(id);
            vku::transition_image(cb, {
                .image = img.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .new_layout = VK_IMAGE_LAYOUT_GENERAL,
                .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
                // TRANSFER in scope: the irradiance atlas is vkCmdClearColorImage-zeroed just below.
                .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                            | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            });
        }
        probe_layouts_initialized_ = true;

        vkCmdFillBuffer(cb, allocator_.get_buffer(probe_active_).buffer, 0, VK_WHOLE_SIZE, 1u);  // default active
        vkCmdFillBuffer(cb, allocator_.get_buffer(probe_offset_).buffer, 0, VK_WHOLE_SIZE, 0u);  // zero relocation

        // Zero the irradiance atlas: relight is amortized behind capture completion, so shading/debug
        // can legally sample texels before their first relight — they must read black, not garbage.
        {
            const ::string::gpu::allocated_image& irr = allocator_.get_image(probe_irrad_);
            const VkClearColorValue zero{ .float32 = { 0, 0, 0, 0 } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, irr.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }

        ProbeCapturePush cp{};
        cp.probe_base = 0; cp.probe_count = total;
        cp.counts_x = counts.x; cp.counts_y = counts.y; cp.counts_z = counts.z;
        cp.cap_gbuf_slot = probe_cap_gbuf_storage_slot_;
        cp.cap_albedo_slot = probe_cap_albedo_storage_slot_;
        cp.vis_slot = probe_vis_storage_slot_;
        cp.far_distance = far_distance;
        const ::string::gpu::pipeline& clr = probe_clear_program_->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, clr.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, clr.pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, clr.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeCapturePush), &cp);
        vkCmdDispatch(cb, (kProbeVisStride + 7) / 8, (kProbeVisStride + 7) / 8, total);
        // clear writes -> subsequent collapse overwrites (and the activation fill) must be visible.
        const VkMemoryBarrier2 mb0 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                           | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
        const VkDependencyInfo dep0 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb0 };
        vkCmdPipelineBarrier2(cb, &dep0);
    }

    const ::string::gpu::allocated_image& cube_alb = allocator_.get_image(probe_cube_albedo_);
    const ::string::gpu::allocated_image& cube_nd = allocator_.get_image(probe_cube_nd_);
    const ::string::gpu::allocated_image& cube_dep = allocator_.get_image(probe_cube_depth_);
    const VkDeviceAddress active_addr = allocator_.get_buffer(probe_active_).device_address;

    // --- Capture, GPU-driven multiview, amortized by PROBE -----------------------------------------
    // All 6 cube faces render in ONE BeginRendering (multiview viewMask=0x3F) over 6-layer array
    // views; the mesh shader picks the per-face view-projection by SV_ViewID. The dispatch is a SINGLE
    // flat vkCmdDrawMeshTasksEXT over all LOD0 meshlets — the task shader resolves each flat index to
    // {draw, meshlet} via the load-time table, GPU frustum-culls against all 6 faces, and amplifies
    // only survivors. NO CPU per-draw loop, NO CPU culling. We capture kProbesPerFrame probes/frame,
    // so a ~250-probe bake finishes in ~20 frames (a couple seconds) with no visible hitch.
    const ::string::gpu::pipeline& rp = probe_capture_raster_program_->current();
    const VkViewport vp = { 0.0f, 0.0f, float(kProbeCubeFace), float(kProbeCubeFace), 0.0f, 1.0f };
    const VkRect2D sc = { { 0, 0 }, { kProbeCubeFace, kProbeCubeFace } };
    const uint32_t task_groups = (probe_total_lod0_meshlets_ + 31u) / 32u;

    // TWO bake rounds (RTXGI-style relocation consistency): round 1 rasters each probe from its
    // GRID position and the collapse derives the relocation offset + classification from that view.
    // Round 2 re-rasters from the RELOCATED position (the raster shader adds offset[probe_index],
    // which reads zero in round 1) and re-collapses, so the visibility/hit distances stored in the
    // atlases are measured from the SAME position every consumer uses via probe_world() — without
    // this, relight reconstructed hit points (and Chebyshev compared distances) up to 0.5*spacing
    // off for every relocated probe: wrongly-shadowed sun taps (under-lit probes) + leak/reject
    // errors at walls. Round 2 keeps the round-1 offset (no drift) but re-votes classification
    // (a probe that escaped a wall can become ACTIVE).
    const uint32_t total_work = total * 2;
    for (uint32_t done = 0; done < kProbesPerFrame && probe_capture_cursor_ < total_work; ++done)
    {
        const uint32_t probe = probe_capture_cursor_ % total;
        const uint32_t round = probe_capture_cursor_ / total;
        const glm::uvec3 c{ probe % counts.x, (probe / counts.x) % counts.y, probe / (counts.x * counts.y) };
        const glm::vec3 probe_pos = probe_volume_.origin + glm::vec3(c) * probe_volume_.spacing;

        // Acquire the cube images into attachment layout (sampled by the previous probe's collapse, or
        // UNDEFINED on the very first capture). All 6 layers at once (multiview target).
        {
            const VkImageLayout old_color = probe_cube_layouts_initialized_
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            for (const ::string::gpu::allocated_image* img : { &cube_alb, &cube_nd })
                vku::transition_image(cb, {
                    .image = img->image, .old_layout = old_color,
                    .new_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    .dst_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .layer_count = 6,
                });
            vku::transition_image(cb, {
                .image = cube_dep.image,
                .old_layout = probe_cube_layouts_initialized_ ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_UNDEFINED,
                .new_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .layer_count = 6,
            });
            probe_cube_layouts_initialized_ = true;
        }

        // Render all 6 faces of this probe's cube G-buffer in ONE multiview pass.
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rp.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rp.pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        {
            const VkRenderingAttachmentInfo color_att[2] = {
                { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                  .imageView = probe_cube_albedo_array_view_,
                  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                  .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } },
                { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                  .imageView = probe_cube_nd_array_view_,
                  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                  .clearValue = { .color = { .float32 = { 0, 0, 0, -1.0f } } } },  // w<0 = sky miss
            };
            const VkRenderingAttachmentInfo depth_att = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = probe_cube_depth_array_view_,
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = { .depthStencil = { 0.0f, 0 } },  // reverse-Z far = 0
            };
            // Multiview: viewMask=0x3F broadcasts each mesh workgroup to all 6 layers; layerCount is
            // ignored (must be 1 per the spec when viewMask != 0).
            const VkRenderingInfo ri = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = sc, .layerCount = 1, .viewMask = kProbeCubeViewMask,
                .colorAttachmentCount = 2, .pColorAttachments = color_att,
                .pDepthAttachment = &depth_att,
            };
            vkCmdBeginRendering(cb, &ri);
            // Push: geometry pointers + the flat-capture table + probe_pos (the 6 face view-projections
            // are built IN the shader from probe_pos — 6 mat4 would blow the 256B push budget).
            struct RasterPush {
                VkDeviceAddress vertices, meshlets, mverts, mtris, draws, mdraw;
                glm::vec3 probe_pos; uint32_t meshlet_count;
                float cull_far; uint32_t probe_index;
                VkDeviceAddress offsets;   // relocation (raster adds offset[probe_index] GPU-side)
            } rpush{};
            static_assert(offsetof(RasterPush, probe_pos) == 48);
            static_assert(offsetof(RasterPush, meshlet_count) == 60);
            static_assert(offsetof(RasterPush, cull_far) == 64);
            static_assert(offsetof(RasterPush, probe_index) == 68);
            static_assert(offsetof(RasterPush, offsets) == 72);
            static_assert(sizeof(RasterPush) == 80);
            rpush.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
            rpush.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
            rpush.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
            rpush.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
            rpush.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
            rpush.mdraw = allocator_.get_buffer(probe_meshlet_draw_).device_address;
            rpush.probe_pos = probe_pos;
            rpush.meshlet_count = probe_total_lod0_meshlets_;
            rpush.cull_far = probe_cull_far_;
            rpush.probe_index = probe;
            rpush.offsets = allocator_.get_buffer(probe_offset_).device_address;
            vkCmdPushConstants(cb, rp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(RasterPush), &rpush);
            vkCmdDrawMeshTasksEXT(cb, task_groups, 1, 1);   // ONE flat GPU-driven dispatch
            vkCmdEndRendering(cb);
        }

        // Cube color -> SHADER_READ for the collapse SamplerCube. (Depth stays an attachment.)
        for (const ::string::gpu::allocated_image* img : { &cube_alb, &cube_nd })
            vku::transition_image(cb, {
                .image = img->image, .old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .layer_count = 6,
            });

        // Collapse this probe's cube into its octahedral atlases + classify + relocate.
        ProbeCapturePush cp{};
        cp.counts_x = counts.x; cp.counts_y = counts.y; cp.counts_z = counts.z;
        cp.cap_gbuf_slot = probe_cap_gbuf_storage_slot_;
        cp.cap_albedo_slot = probe_cap_albedo_storage_slot_;
        cp.vis_slot = probe_vis_storage_slot_;
        cp.far_distance = far_distance;
        cp.cube_albedo_slot = probe_cube_albedo_sample_slot_;
        cp.cube_nd_slot = probe_cube_nd_sample_slot_;
        cp.probe_pos = probe_pos;
        cp.probe_index = probe;
        cp.round2 = round;
        cp.active = active_addr;
        cp.spacing = probe_volume_.spacing;
        cp.offset = allocator_.get_buffer(probe_offset_).device_address;
        const ::string::gpu::pipeline& col = probe_collapse_program_->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, col.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, col.pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, col.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeCapturePush), &cp);
        vkCmdDispatch(cb, 1, 1, 1);   // one workgroup (18x18) per probe

        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            // TASK/MESH: the round-2 capture raster reads the offset buffer (relocated centre) in
            // its task/mesh/fragment stages — the collapse's offset write must be visible there.
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                          | VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        vkCmdPipelineBarrier2(cb, &dep);

        ++probe_capture_cursor_;
    }

    probe_capture_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (probe_capture_cursor_ >= total * 2 && !probe_captured_)
    {
        probe_captured_ = true;
        STRING_LOG_INFO("[gi] probe capture complete: {} probes x 2 rounds (relocate + re-capture), "
                        "{:.1f} ms CPU-record over frames",
                        total, probe_capture_ms_);
    }
}

// M2 dynamic relight -> irradiance atlas: capture-driven radiance (miss -> live sky; hit -> albedo
// x (1-tap-CSM-shadowed sun + bounce from the previous atlas)) cosine-convolved per octa texel with
// hysteresis. AMORTIZED: kRelightProbesPerFrame per frame, round-robin; a sun trigger arms
// kRelightConvergePasses full passes (each pass propagates the bounce one step; the hysteresis EMA
// settles to h^N), then relight goes idle (~0 static cost). Under TOD animation the trigger re-arms
// every frame, so the volume tracks the sun continuously.
void GeometryPass::record_probe_relight(::string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (!probe_volume_.valid || probe_relight_program_ == nullptr || ibl->sh_buffer() == 0
        || !scene_buffer_.valid() || current_frame >= frames_in_flight_)
        return;
    VkCommandBuffer cb = recorder.vk();   // probe relight = compute + barriers raw Vulkan (vk() escape)
    VkDescriptorSet set = descriptor_table_.get_set();

    // RAW: relight reads the sky-SH buffer at COMPUTE. The IBL chain (recorded just before, same CB)
    // ends its SH write with a FRAGMENT-only barrier, so make the SH write visible to this compute
    // read here (intra-pass, like the IBL local barrier class). Harmless when SH is unchanged.
    {
        const VkMemoryBarrier2 sh_raw = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &sh_raw };
        vkCmdPipelineBarrier2(cb, &dep);
    }

    // Cross-frame WAR: last frame's fragment reads of the irradiance atlas must retire before this
    // rewrite (execution dependency; the atlas lives permanently in GENERAL).
    if (probe_primed_)
    {
        const VkMemoryBarrier2 war = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &war };
        vkCmdPipelineBarrier2(cb, &dep);
    }

    const uint32_t total = probe_volume_.total();
    const uint32_t base = probe_relight_cursor_;
    const uint32_t count = std::min(kRelightProbesPerFrame, total - base);

    ProbeRelightPush push{};
    push.origin_spacing = probe_origin_spacing(probe_volume_);
    push.spacing = glm::vec4(probe_volume_.spacing, 0.0f);
    push.counts = glm::uvec4(probe_volume_.counts, total);
    push.sun_dir = glm::vec4(glm::normalize(sun_dir_), sun_intensity_);
    push.sun_color = glm::vec4(sun_color_, probe_primed_ ? std::clamp(cv_gi_hysteresis().get(), 0.0f, 0.99f) : 0.0f);
    push.sky_zenith = glm::vec4(sky_zenith_, 0.0f);
    push.sky_ground = glm::vec4(sky_ground_, 0.0f);
    push.cap_gbuf_slot = probe_cap_gbuf_sample_slot_;
    push.cap_albedo_slot = probe_cap_albedo_sample_slot_;
    push.irrad_prev_slot = probe_irrad_sample_slot_;
    push.irrad_dst_slot = probe_irrad_storage_slot_;
    push.vis_slot = probe_vis_sample_slot_;
    push.first_frame = probe_primed_ ? 0u : 1u;
    push.probe_base = base;
    push.probe_count = count;
    push.sh = ibl->sh_address();
    push.active = allocator_.get_buffer(probe_active_).device_address;
    push.offsets = allocator_.get_buffer(probe_offset_).device_address;
    push.scene = resources->address(scene_buffer_, current_frame);

    const ::string::gpu::pipeline& p = probe_relight_program_->current();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ProbeRelightPush), &push);
    // One workgroup (10x10 threads) per probe in this frame's slice; z = probe.
    vkCmdDispatch(cb, 1, 1, count);

    // Relight writes -> shading + debug + next-frame bounce reads. Also an EXECUTION edge to the
    // depth-test stages: relight's 1-tap sun shadow READ this slot's cascade shadow maps at COMPUTE,
    // and the shadow pass recorded later this frame WRITES them (WAR — no memory flush needed).
    const VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                      | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                       | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    vkCmdPipelineBarrier2(cb, &dep);

    // Advance the round-robin cursor; a completed pass consumes one armed converge pass.
    probe_relight_cursor_ = base + count;
    if (probe_relight_cursor_ >= total)
    {
        probe_relight_cursor_ = 0;
        probe_primed_ = true;
        probe_relit_sun_dir_ = glm::normalize(sun_dir_);
        if (probe_relight_passes_left_ > 0) --probe_relight_passes_left_;
        probe_relight_pending_ = probe_relight_passes_left_ > 0;
    }
}

// Instanced probe-debug spheres, drawn into the (already-open) scene MSAA render pass after opaque
// geometry (depth-tested). One instance per probe; the sphere is procedural (no vertex buffer).
void GeometryPass::record_probe_debug(::string::gpu::command_recorder& recorder)
{
    if (!probe_volume_.valid || probe_debug_program_ == nullptr || probe_debug_mode_ == 0) return;
    VkCommandBuffer cb = recorder.vk();   // instanced probe-debug raster raw Vulkan (vk() escape)
    VkDescriptorSet set = descriptor_table_.get_set();
    const ::string::gpu::pipeline& p = probe_debug_program_->current();

    ProbeDebugPush push{};
    push.view_proj = camera_.view_proj();
    const float min_sp = std::min(probe_volume_.spacing.x,
                                  std::min(probe_volume_.spacing.y, probe_volume_.spacing.z));
    push.origin_spacing = glm::vec4(probe_volume_.origin, min_sp * 0.15f);   // sphere radius
    push.spacing = glm::vec4(probe_volume_.spacing, 0.0f);
    push.counts = glm::uvec4(probe_volume_.counts, probe_debug_mode_);   // 1 grey, 2 irradiance, 3 vis
    push.camera_pos = glm::vec4(camera_.position(), String::CompositePass::exposure_scale());
    push.irrad_slot = probe_irrad_sample_slot_;
    push.vis_slot = probe_vis_sample_slot_;
    push.active = allocator_.get_buffer(probe_active_).device_address;
    push.offset = allocator_.get_buffer(probe_offset_).device_address;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, p.pipeline_layout, p.push_constants.stageFlags, 0, sizeof(ProbeDebugPush), &push);
    // rings*sectors*6 verts per sphere (kRings=8, kSectors=12 -> 576), one instance per probe.
    vkCmdDraw(cb, 8u * 12u * 6u, probe_volume_.total(), 0, 0);
}

}  // namespace string::render
