#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <string/gpu/resource.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/ui/dynamic_font.hpp>

#include <string/scene/asset_registry.hpp>
#include <string/scene/world.hpp>

#include <string/render/scene_bridge.hpp>
#include <string/render/scene_uniforms.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/shadow_maps.hpp>
#include <string/render/sorted_transparency.hpp>
#include <string/render/gtao.hpp>
#include <string/render/probe_gi_component.hpp>
#include <string/render/geometry/sky_component.hpp>
#include <string/render/geometry/froxel_component.hpp>
#include <string/render/geometry/ibl_component.hpp>
#include <string/render/debug_line_pass.hpp>
#include <string/render/post_pass.hpp>
#include <string/render/ui_pass.hpp>

namespace string::render
{

struct scene_resources
{
    string::gpu::image swapchain, color, depth, hdr;
    string::gpu::image hiz_depth, hiz_pyramid;
    string::gpu::image gtao_raw, gtao_ao;
    string::gpu::image bloom;
    string::gpu::image shadow[kMaxCascades];
    string::gpu::image env_capture, env_prefiltered, dfg_lut;
    string::gpu::image hiz_depth_prev;
    string::gpu::image gi_irradiance, gi_cap_gbuf, gi_cap_albedo, gi_visibility;
    string::gpu::image gi_cube_albedo, gi_cube_nd, gi_cube_depth;
    string::gpu::buffer froxels, ibl_sh, transparency_list, histogram_bins;
    WorklistSet worklists;
    string::gpu::buffer scene_data, lights, stats;
    string::gpu::buffer gi_active, gi_offset, gi_meshlet_table;
    string::gpu::buffer joint_palette;   // brief 23: the anim tick's palette ring
    uint32_t bloom_mips = 0;
};

// Brief 21 D3 — time lives OUTSIDE the graph. Every resource whose CONTENTS outlive the frame
// (cross-frame history, host-write pacing rings, in-place accumulators) has a lifetime beyond the
// graph's scope, so the APP creates and destroys its backing and hands it in as a persistent; the
// graph tracks usage only. This struct is that owner for the geometry scene. The category test:
// contents outlive the frame -> persistent (here); frame-scoped -> transient (fg.image/fg.buffer).
struct scene_backing
{
    string::engine_context* ctx = nullptr;
    // The depth-history ring: GTAO reprojection reads LAST frame's resolved depth, so the app backs
    // one image per frame in flight and declares TWO handles over them — identity order for this
    // frame's write, rotated one slot back for last frame's read. The rotation is static: resolved
    // by frame slot, no per-frame swap call, no reach into graph records.
    std::vector<string::gpu::resource_id> hiz_depth;
    // Host-write pacing rings: the CPU fills frame N+1's slot while the GPU still reads frame N's.
    std::vector<string::gpu::resource_id> scene_data, lights, stats, transparency_list;
    // Brief 23: the joint-palette ring — the anim tick writes slot N+1's palettes while the GPU
    // skins with slot N's. Identical layout every slot (palette offsets are load-time constants;
    // only the base address rotates, through SceneData).
    std::vector<string::gpu::resource_id> joint_palette;
    // Probe-GI accumulators: captured/relit tiles persist across the amortized rounds, and readers
    // must see REAL accumulated data on frames when no producer pass runs — as transients they were
    // degrade-substituted with the 1x1 neutral on exactly those frames.
    string::gpu::resource_id gi_irradiance = 0, gi_cap_gbuf = 0, gi_cap_albedo = 0, gi_visibility = 0;
    string::gpu::resource_id gi_active = 0, gi_offset = 0, gi_meshlet_table = 0;
    // IBL products: the bake is AMORTIZED — it re-runs only when the sun moves, so on nearly every
    // frame nothing writes them and everything reads them. Same category as the GI atlases, and the
    // same failure when they were transients: a resize destroyed their backing, no bake re-ran, and
    // the whole scene shaded against 1x1 neutral env/DFG until the sun happened to move.
    string::gpu::resource_id env_capture = 0, env_prefiltered = 0, dfg_lut = 0, ibl_sh = 0;

    void create(string::engine_context& c, VkExtent2D viewport, uint32_t max_draws,
                const ProbeVolume& gi_volume, std::size_t gi_table_entries,
                uint32_t palette_joints)
    {
        ctx = &c;
        const uint32_t slots = c.frames_in_flight;

        create_hiz_depth(viewport);

        const VkBufferUsageFlags host_ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const VmaAllocationCreateFlags mapped = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        const auto ring = [&](std::vector<string::gpu::resource_id>& out, string::gpu::buffer_info info) {
            for (uint32_t s = 0; s < slots; ++s) out.push_back(c.allocator.create_resource(info));
        };
        ring(scene_data, { .size = sizeof(SceneData), .usage = host_ssbo,
                           .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU, .allocation_flags = mapped });
        ring(lights, { .size = sizeof(GpuLight) * 1024u, .usage = host_ssbo,
                       .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU, .allocation_flags = mapped });
        ring(stats, { .size = sizeof(GpuMeshStats),
                      .usage = host_ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
                      .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT });
        ring(transparency_list, { .size = sorted_transparency::layout_for(max_draws).bytes,
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                         | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                         | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                  .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                                  .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                                    | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT });
        // Brief 23: sized to the loaded scene's total palette joints (a scene with no skins gets
        // one identity-sized slot so the handle stays valid and the graph stays static).
        ring(joint_palette, { .size = sizeof(glm::mat4) * std::max(1u, palette_joints),
                              .usage = host_ssbo,
                              .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                              .allocation_flags = mapped });
        // Every slot defaults to IDENTITY palettes — an identity palette skins the bind pose (the
        // skinned vertices are authored in bind space), so a character whose animation never
        // arrives (no driver yet, skeleton-hash mismatch) stands in bind pose instead of
        // rendering whatever VMA recycled into the ring.
        for (string::gpu::resource_id id : joint_palette)
        {
            auto* mats = static_cast<glm::mat4*>(
                c.allocator.get_buffer(id).allocation_info.pMappedData);
            for (uint32_t j = 0; j < std::max(1u, palette_joints); ++j) mats[j] = glm::mat4(1.0f);
        }

        const glm::uvec2 gi_tiles = gi_volume.tile_grid();
        const string::gpu::sampler_info gi_sampler{ .mag_filter = VK_FILTER_LINEAR,
                                                    .min_filter = VK_FILTER_LINEAR,
                                                    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                                    .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                    .anisotropy = false };
        const auto gi_atlas = [&](uint32_t stride) {
            string::gpu::image_info info{};
            info.extent = { std::max(1u, gi_tiles.x * stride), std::max(1u, gi_tiles.y * stride), 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = gi_sampler;
            return c.allocator.create_resource(info);
        };
        gi_irradiance = gi_atlas(kProbeIrradStride);
        gi_cap_gbuf   = gi_atlas(kProbeVisStride);
        gi_cap_albedo = gi_atlas(kProbeVisStride);
        gi_visibility = gi_atlas(kProbeVisStride);

        const VkBufferUsageFlags gi_buf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        gi_active = c.allocator.create_resource(string::gpu::buffer_info{
            .size = std::max<VkDeviceSize>(4 * gi_volume.total(), 4), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
        gi_offset = c.allocator.create_resource(string::gpu::buffer_info{
            .size = std::max<VkDeviceSize>(16 * gi_volume.total(), 16), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
        gi_meshlet_table = c.allocator.create_resource(string::gpu::buffer_info{
            .size = 8 * std::max<VkDeviceSize>(gi_table_entries, 1), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });

        // IBL: two 128^3 cubes with a 6-mip roughness ladder, the split-sum BRDF LUT, and the SH
        // buffer. Viewport-INDEPENDENT, so a resize leaves them alone — which is the point: the bake
        // that fills them is amortized and will not re-run just because the window changed.
        const string::gpu::sampler_info env_sampler{
            .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
            .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .anisotropy = false,
            .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
        const auto env_cube = [&] {
            string::gpu::image_info info{};
            info.extent = { 128, 128, 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = env_sampler;
            info.mip_levels = 6;
            info.cube = true;   // 6 array layers + CUBE_COMPATIBLE, per image_info::cube
            return c.allocator.create_resource(info);
        };
        env_capture = env_cube();
        env_prefiltered = env_cube();
        {
            string::gpu::image_info info{};
            info.extent = { 128, 128, 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = env_sampler;
            dfg_lut = c.allocator.create_resource(info);
        }
        ibl_sh = c.allocator.create_resource(string::gpu::buffer_info{
            .size = sizeof(float) * 4 * 9,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
    }

    // The depth-history ring is viewport-sized, so ITS OWNER re-creates it on resize (the graph
    // only re-backs what it owns — the viewport-scaled transients). Called at creation and from the
    // renderer's resize callback, under device idle.
    void create_hiz_depth(VkExtent2D viewport)
    {
        for (string::gpu::resource_id id : hiz_depth) ctx->allocator.destroy_resource(id);
        hiz_depth.clear();
        string::gpu::image_info depth_info{};
        depth_info.extent = { viewport.width, viewport.height, 1 };
        depth_info.format = VK_FORMAT_D32_SFLOAT;
        depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        depth_info.aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
        for (uint32_t s = 0; s < ctx->frames_in_flight; ++s)
            hiz_depth.push_back(ctx->allocator.create_resource(depth_info));
    }

    ~scene_backing()
    {
        if (ctx == nullptr) return;
        for (string::gpu::resource_id id : hiz_depth) ctx->allocator.destroy_resource(id);
        for (auto* v : { &scene_data, &lights, &stats, &transparency_list, &joint_palette })
            for (string::gpu::resource_id id : *v) ctx->allocator.destroy_resource(id);
        for (string::gpu::resource_id id : { gi_irradiance, gi_cap_gbuf, gi_cap_albedo, gi_visibility,
                                             gi_active, gi_offset, gi_meshlet_table,
                                             env_capture, env_prefiltered, dfg_lut, ibl_sh })
            if (id != 0) ctx->allocator.destroy_resource(id);
    }
};

// The graph resources for the forward technique stack (moved from the demo with the
// forward_renderer preset — the TYPE stays public: lighter scenes declare a subset inline).
scene_resources declare_resources(string::frame_graph& fg, VkExtent2D viewport,
                                  VkSampleCountFlagBits samples, uint32_t shadow_res,
                                  uint32_t cascades, VkDeviceSize worklist_bytes,
                                  VkDeviceSize draw_lod_bytes, const scene_backing* backing);

// Per-frame streaming uploads, INSIDE the frame (brief 21 step 5). EVERY SCENE MUST DECLARE THIS.
void declare_stream_uploads(string::frame_graph& fg, string::engine_context& ctx);

// THE PRESET: the full forward+ pass stack as ONE app-ownable object. Reinstates brief 18's
// deleted make_geometry_setup intent WITHOUT reintroducing RenderPlan or a pass base class — the
// app still owns the frame_graph; this declares into it. It absorbs what used to be ~1000 lines
// of per-app wiring: the persistent backing, every resource declaration, 13 components, the
// LOAD-BEARING declaration order (ibl -> gi -> shadow), the capture + resize hooks, and the
// renderer half of the frame tick. The app keeps: the world (content + camera + environment),
// input, and WHAT to render (its UI authors, its crowd/content hooks).
class forward_renderer
{
public:
    struct ui_hooks
    {
        std::shared_ptr<string::dynamic_font_atlas> atlas;
        ui_pass::Author author;
        ui_pass::PostLayout post_layout;
        ui_pass::DeferredAuthor deferred;
    };
    struct settings
    {
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
        VkExtent2D viewport{ 0, 0 };
        VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
        bool lookdev = false;
        // Spawn budgets; 0 = derive from the loaded content (parts x 36, joints x 36 — the
        // stress-grid default).
        uint32_t row_budget = 0;
        uint32_t palette_budget = 0;
    };

    forward_renderer(engine_context& ctx, ::string::renderer& rr,
                     ::string::assets::registry& assets, ::string::scene::world& world,
                     const settings& s);
    // The app's UI closures (they typically capture overlay_stats(), so this is a setter, not a
    // ctor arg). Must be called before declare().
    void set_ui(ui_hooks ui) { ui_hooks_ = std::move(ui); }
    ~forward_renderer();

    // Declare EVERYTHING onto the app's graph: the registry heaps, the persistent backing, every
    // resource, every pass in the correct order, the capture target and the resize hook.
    void declare(::string::frame_graph& fg);

    // The renderer half of the frame (call after the app ticks the world): bridge re-derive,
    // palette writes, then every component tick in order.
    void tick(float dt);

    scene_bridge& bridge() { return *bridge_; }
    const scene_bridge& bridge() const { return *bridge_; }
    void set_crowd_hook(std::function<void(bool)> hook);
    std::shared_ptr<MeshOverlayStats> overlay_stats() const { return mesh_stats_; }

private:
    engine_context& ctx_;
    ::string::renderer& rr_;
    ::string::assets::registry& assets_;
    ::string::scene::world& world_;
    settings settings_;
    ui_hooks ui_hooks_;

    scene_backing backing_;
    std::shared_ptr<MeshOverlayStats> mesh_stats_ = std::make_shared<MeshOverlayStats>();
    std::unique_ptr<scene_bridge> bridge_;
    std::unique_ptr<geometry_pass> geo_;
    std::unique_ptr<froxel_component> froxel_;
    std::unique_ptr<ibl_component> ibl_;
    std::unique_ptr<gtao_chain> gtao_;
    std::unique_ptr<probe_gi_component> gi_;
    std::unique_ptr<sky_component> sky_;
    std::unique_ptr<shadow_maps> shadow_;
    std::unique_ptr<sorted_transparency> transparency_;
    std::unique_ptr<scene_uniforms> uniforms_;
    std::unique_ptr<debug_line_pass> debug_lines_;
    std::unique_ptr<ui_pass> ui_;
    std::unique_ptr<post_pass> post_;
    std::unique_ptr<::string::composite_pass> composite_;
    scene_resources res_;
    ::string::frame_graph* graph_ = nullptr;
};

}  // namespace string::render
