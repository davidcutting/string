// meshlet task/mesh draw path + GPU draw-cull/expand + crowd — split out of geometry_pass.cpp (brief 11 modularization). These remain GeometryPass
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

#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace String;

void GeometryPass::build_meshlet_gpu(engine_context& context)
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
        if (info.flags & kDrawFlagBlend) blend_draw_indices_.push_back(static_cast<uint32_t>(i));
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

    // GPU-written stats, read back one frame late (registry-owned PerFrame host-visible ring; brief 16 M3).
    stats_readback_.resize(frames_in_flight_);
    stats_buffer_ = resources->create_per_frame(::string::gpu::buffer_info{
        .size = sizeof(GpuMeshStats),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    }, frames_in_flight_);

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
    wl_offsets_off_   = align16(counts_bytes);
    wl_blocksums_off_ = align16(wl_offsets_off_ + offsets_bytes);
    wl_commands_off_  = align16(wl_blocksums_off_ + blocksums_bytes);
    wl_records_off_   = align16(wl_commands_off_ + commands_bytes);
    wl_count_off_     = align16(wl_records_off_ + records_bytes);
    const VkDeviceSize worklist_size = wl_count_off_ + 16;   // count word (16B-padded)

    // Brief 04e M3: worklists + the shared draw_lod are per-frame TRANSIENTS (fully rebuilt by
    // the draw/expand computes every frame), so they live in the renderer's per-frame-slot
    // scratch arena instead of 21 dedicated allocations (6 worklists + draw_lod, x3 slots).
    // reserve() returns the region's offset (identical in every slot's buffer); the slot buffer
    // ids are bound lazily in update() once the renderer materializes the arena.
    const auto reserve_worklist = [&]() -> Worklist {
        return Worklist{ 0, scratch_->reserve(worklist_size) };
    };
    wl_opaque_.resize(frames_in_flight_);
    wl_twosided_.resize(frames_in_flight_);
    wl_shadow_.resize(frames_in_flight_);
    {
        const Worklist opaque = reserve_worklist();
        const Worklist twosided = reserve_worklist();
        std::array<Worklist, kMaxCascades> shadow{};
        for (uint32_t c = 0; c < kMaxCascades; ++c) shadow[c] = reserve_worklist();
        draw_lod_off_ = scratch_->reserve(u32 * max_draws);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            wl_opaque_[f] = opaque;
            wl_twosided_[f] = twosided;
            wl_shadow_[f] = shadow;
        }
    }

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

    // Brief 04c transparency list: host-visible (CPU sorts + fills each frame) in the SAME compacted
    // per-draw shape the task shader consumes — commands[] (12B) @0, records[] (8B), then the count
    // word. Draws written back-to-front (CPU sort preserved), one command+record per surviving draw.
    transp_commands_off_ = 0;
    transp_records_off_  = align16(VkDeviceSize(sizeof(uint32_t) * 3) * max_draws);
    transp_count_off_    = align16(transp_records_off_ + VkDeviceSize(sizeof(uint32_t) * 2) * max_draws);
    const VkDeviceSize transp_size = transp_count_off_ + 16;
    transp_buffers_.resize(frames_in_flight_);
    transp_mapped_.resize(frames_in_flight_);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        transp_buffers_[f] = allocator_.create_resource(::string::gpu::buffer_info{
            .size = transp_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        });
        transp_mapped_[f] = static_cast<uint32_t*>(
            allocator_.get_buffer(transp_buffers_[f]).allocation_info.pMappedData);
    }

    // --- Pipelines (all through the hot-reload registry) ---
    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    VkSampleCountFlagBits samples = context.sample_count;

    // Task/mesh/fragment lit meshlet pipeline. Same fixed state as the lit vertex pipeline (back-face
    // cull, CCW, MSAA, reverse-Z depth, alpha blend), but built via build_mesh_pipeline (no VI state).
    // Brief 04: three variants off the same source differ only in fixed state:
    //   - one-sided opaque: CULL_BACK, depth write ON;
    //   - two-sided opaque: CULL_NONE, depth write ON;
    //   - transparency:     CULL_NONE, depth TEST vs opaque depth but NO write (sorted back-to-front
    //     on the CPU, so the blend order is correct without depth writes). blend_pass push flag makes
    //     the fragment output base-color alpha.
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
    meshlet_transparent_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "meshlet_mesh.slang", make_lit(VK_CULL_MODE_NONE, /*depth_write*/ false));

    // Depth-only meshlet shadow pipeline (no cull; depth bias handles acne, off-frustum casters kept).
    meshlet_shadow_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "meshlet_shadow.slang",
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

    // HiZ sampler: nearest + clamp (min-reduced pyramid; we sample explicit mips).
    const VkSamplerCreateInfo hiz_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    if (vkCreateSampler(device_.get_device(), &hiz_sampler_info, nullptr, &hiz_sampler_) != VK_SUCCESS)
        throw std::runtime_error("GeometryPass: failed to create HiZ sampler");
    hiz_.resize(frames_in_flight_);
}

void GeometryPass::build_crowd(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
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

void GeometryPass::record_draw_cull(::string::gpu::command_recorder& recorder, uint16_t current_frame, int cascade)
{
    // Brief 04c DRAW PHASE. cascade < 0: camera — writes the opaque + two-sided per-draw meshlet
    // counts (into wl_opaque/wl_twosided counts@0) + the shared draw_lod. cascade >= 0: that
    // cascade's shadow list — writes counts_shadow into wl_shadow[c] counts@0, rejecting whole draws
    // outside the CASCADE's light sphere (never the camera frustum). (Brief 04d deleted the old
    // depth-only HiZ camera prepass, so there is no forced-LOD0 prepass variant here anymore.)
    const bool shadow = cascade >= 0;
    const VkDeviceAddress opaque_base =
        allocator_.get_buffer(wl_opaque_[current_frame].buffer).device_address + wl_opaque_[current_frame].offset;
    const VkDeviceAddress twosided_base =
        allocator_.get_buffer(wl_twosided_[current_frame].buffer).device_address + wl_twosided_[current_frame].offset;
    const VkDeviceAddress shadow_base =
        shadow ? allocator_.get_buffer(wl_shadow_[current_frame][cascade].buffer).device_address + wl_shadow_[current_frame][cascade].offset : 0;

    const float lod_error_px = cv_lod_error_px().get();   // CVar r.lod.error_px (STRING_LOD_PX alias)
    const float half_h = screen_size.height * 0.5f;
    const float focal = half_h / std::tan(glm::radians(camera_.fov_degrees() * 0.5f));

    const ::string::gpu::pipeline& cp = draw_cull_program_->current();
    DrawCullPush cull{};
    // Freeze-aware inputs: when F is held EVERY camera-derived cull input comes from the frozen pose.
    cull.cull_view_proj = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : camera_.view_proj();
    cull.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    if (shadow)
    {
        // Shadow only consumes counts_shadow (counts@0 of wl_shadow[c]); the opaque/two-sided writes
        // land in this same buffer's scratch regions (overwritten by its own expand before use).
        cull.counts_shadow    = shadow_base;                       // counts@0
        cull.counts           = shadow_base + wl_offsets_off_;     // throwaway (expand recomputes)
        cull.counts_twosided  = shadow_base + wl_blocksums_off_;   // throwaway
    }
    else
    {
        cull.counts          = opaque_base;      // counts@0 of the opaque worklist
        cull.counts_twosided = twosided_base;    // counts@0 of the two-sided worklist
        cull.counts_shadow   = opaque_base + wl_offsets_off_;   // throwaway (camera pass ignores it)
    }
    // Only the CAMERA opaque/two-sided pass writes the shared camera-selected draw_lod (the main pass +
    // shadow compaction records read it). The shadow passes must NOT re-write it — a redundant second
    // writer creates a write-after-write hazard on the shared buffer for no benefit (shadow compaction
    // reuses the camera draw_lod). Shadow writes its throwaway per-draw LODs into a scratch region (the
    // commands[] area, overwritten by its own fill before use) instead.
    cull.draw_lod = shadow
        ? shadow_base + wl_commands_off_   // shadow throwaway -> commands (fill overwrites)
        : draw_lod_address(current_frame);
    cull.stats = resources->address(stats_buffer_, current_frame);
    cull.draw_count = active_draw_count_;
    cull.lod_enabled = lod_enabled_ ? 1u : 0u;
    cull.force_lod0 = 0u;                                       // (dead: brief 04d deleted the LOD0 prepass)
    cull.isolate_draw = cv_isolate_draw().get();               // brief 06: >=0 keeps only that draw
    cull.frustum_cull = cull_enabled_ ? 1u : 0u;              // C toggle disables DRAW-level camera cull
    cull.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    cull.lod_error_px = lod_error_px;
    cull.focal = focal;
    // Histogram only on the camera opaque/twosided pass (not shadow) — matches pre-04c meaning.
    cull.stats_lod = shadow ? 0u : 1u;
    cull.shadow_mode = (shadow && cull_enabled_) ? 1u : 0u;
    if (shadow)
    {
        cull.shadow_center = cascade_center_[cascade];
        cull.shadow_radius = cascade_cull_radius_[cascade];
    }
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, cp.pipeline);
    recorder.push_constants(cp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(DrawCullPush), &cull);
    recorder.dispatch((active_draw_count_ + 63) / 64, 1, 1);

    // Counts + draw_lod must be visible to the expansion compute (storage read).
    const VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    recorder.barrier(dep);
}

void GeometryPass::record_expand(::string::gpu::command_recorder& recorder, const Worklist& wl, VkDeviceAddress draw_lod)
{
    const VkDeviceAddress base = allocator_.get_buffer(wl.buffer).device_address + wl.offset;
    ExpandPush push{};
    push.counts     = base;
    push.offsets    = base + wl_offsets_off_;
    push.block_sums = base + wl_blocksums_off_;
    push.draw_lod   = draw_lod;
    push.commands   = base + wl_commands_off_;
    push.records    = base + wl_records_off_;
    push.count      = base + wl_count_off_;
    push.draw_count = active_draw_count_;
    push.block_count = (active_draw_count_ + kScanBlock - 1) / kScanBlock;
    push.max_draws  = cull_max_draws_;

    const auto storage_barrier = [&]() {
        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        recorder.barrier(dep);
    };

    const auto dispatch = [&](::string::gpu::shader_program* prog, uint32_t groups) {
        const ::string::gpu::pipeline& p = prog->current();
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ExpandPush), &push);
        recorder.dispatch(groups, 1, 1);
    };

    dispatch(expand_scan_blocks_program_, push.block_count);   // one workgroup per block
    storage_barrier();
    dispatch(expand_scan_carry_program_, 1);                   // single-workgroup block-carry scan
    storage_barrier();
    dispatch(expand_fill_program_, (active_draw_count_ + 63) / 64);
    // Entries[] + command visible to the indirect draw + task shader.
    const VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    recorder.barrier(dep);
}

void GeometryPass::record_meshlet_draws(::string::gpu::command_recorder& recorder, const ::string::gpu::pipeline& p,
                                        uint16_t current_frame, const Worklist& wl, uint32_t phase)
{
    const glm::mat4 vp = camera_.view_proj();
    const glm::mat4 cull_vp = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : vp;
    const HizPyramid& hz = hiz_[current_frame];
    const bool hiz_ready = hiz_enabled_ && hz.image != 0;
    const VkDeviceAddress base = allocator_.get_buffer(wl.buffer).device_address + wl.offset;

    MeshletPush push{};
    push.view_proj = vp;
    push.cull_view_proj = cull_enabled_ ? cull_vp : vp;   // (frustum planes; C disables via wide test)
    push.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
    push.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
    push.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
    push.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
    push.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    push.scene = resources->address(scene_buffer_, current_frame);
    push.stats = resources->address(stats_buffer_, current_frame);
    push.records = base + wl_records_off_;
    // Frozen-aware eye: cone backface + HiZ nearest-point tests must use the SAME eye the frustum
    // froze from, or freeze-cull mixes live/frozen inputs (visible as bogus culling when flying).
    push.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    push.debug_view = static_cast<uint32_t>(debug_view_);
    // Brief 04d: phase 1 renders bit-set (last-frame-visible) meshlets with NO HiZ test (the pyramid
    // isn't built yet); phase 2 tests the bit-clear complement against the freshly-built pyramid. Phase
    // 0 is the legacy single-pass (HiZ-off) path. Only phase 2 needs the pyramid slot.
    const bool use_hiz = hiz_ready && phase != 1u;
    push.hiz_slot = use_hiz ? hz.sample_slot : 0;
    push.hiz_mips = use_hiz ? hz.mips : 0;
    push.hiz_size = use_hiz ? hz.size : glm::uvec2(1, 1);
    push.blend_pass = 0u;
    push.phase = phase;
    push.bitfield = allocator_.get_buffer(visbits_buffer_).device_address;
    push.freeze_bits = mesh_cull_frozen_ ? 1u : 0u;
    const VkBuffer buf = allocator_.get_buffer(wl.buffer).buffer;

    recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    recorder.draw_mesh_tasks_indirect_count(buf, wl.offset + wl_commands_off_, buf, wl.offset + wl_count_off_,
                                            cull_max_draws_, sizeof(uint32_t) * 3);
}

void GeometryPass::record_opaque_phase(::string::gpu::command_recorder& recorder, uint16_t current_frame, uint32_t phase)
{
    const ::string::gpu::pipeline& mp = meshlet_program_->current();
    VkDescriptorSet mset = descriptor_table_.get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mp.pipeline_layout, 0, 1, &mset, 0, nullptr);
    record_meshlet_draws(recorder, mp, current_frame, wl_opaque_[current_frame], phase);
    if (meshlet_twosided_program_)
    {
        const ::string::gpu::pipeline& tp = meshlet_twosided_program_->current();
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      tp.pipeline_layout, 0, 1, &mset, 0, nullptr);
        record_meshlet_draws(recorder, tp, current_frame, wl_twosided_[current_frame], phase);
    }
}

}  // namespace string::render
