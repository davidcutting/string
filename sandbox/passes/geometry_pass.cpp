#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <string/core/cache_dir.hpp>
#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include "geometry_pass.hpp"
#include "../debug_cvars.hpp"
#include <glm/gtc/constants.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/resource.hpp"
#include "vulkan/vulkan_core.h"

// The single stb_image implementation for the sandbox lives here (this pass decodes textures).
#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

// libktx: cooked .ktx2 textures are already BC7 (see tools/cook_textures.sh) and stream in without
// any CPU pixel decode or transcode — the fast path that avoids the stb_image load-time floor.
// ktxvulkan.h (VkFormat query) requires the Vulkan headers above it and pulls in ktx.h itself.
#include <ktxvulkan.h>

namespace sandbox
{
using namespace String;

namespace
{

// A texture decoded to RGBA8 on a worker thread, ready for a (single-threaded) GPU upload. The
// pixels are owned by stb_image and freed via its own deallocator when this is destroyed, so
// decode jobs can run in parallel without an extra copy.
struct DecodedTexture
{
    struct StbiDeleter { void operator()(stbi_uc* p) const { stbi_image_free(p); } };
    std::unique_ptr<stbi_uc, StbiDeleter> pixels;
    int width = 0;
    int height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
};

// The coarse-tail detail (resident mip levels from the coarsest) whose finest level is ~floor_px
// wide — the LOD floor every streamed texture is pinned to. detail == levels means all mips.
std::uint32_t coarse_detail_for(std::uint32_t levels, std::uint32_t base_extent, std::uint32_t floor_px)
{
    std::uint32_t base_mip = base_extent <= floor_px
        ? 0u
        : static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(base_extent) / floor_px)));
    base_mip = std::min(base_mip, levels - 1);
    return levels - base_mip;
}

// Rough screen-coverage LOD: project the draw's world AABB, take its largest screen-space pixel
// span, and pick the mip whose texel count matches that span (aim for texel:pixel ~ 1). Returns a
// detail in [coarse_detail, levels]. Deliberately cheap and per-draw (v1 CPU feedback) — no UVs, so
// it assumes the texture maps ~once across the surface, which is fine for a first pass.
std::uint32_t desired_detail(const glm::mat4& view_proj, const glm::vec3& mn, const glm::vec3& mx,
                             VkExtent2D screen, std::uint32_t levels, std::uint32_t base_extent,
                             std::uint32_t coarse_detail)
{
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(std::numeric_limits<float>::lowest());
    int in_front = 0;
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec4 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f);
        const glm::vec4 clip = view_proj * corner;
        if (clip.w <= 1e-4f)
        {
            continue;  // behind / on the near plane — skip (a projected point would be meaningless)
        }
        ++in_front;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 px = (ndc * 0.5f + 0.5f) * glm::vec2(screen.width, screen.height);
        lo = glm::min(lo, px);
        hi = glm::max(hi, px);
    }
    if (in_front == 0)
    {
        return coarse_detail;
    }
    const float span = std::max(hi.x - lo.x, hi.y - lo.y);
    if (span <= 1.0f)
    {
        return coarse_detail;
    }
    // base_extent texels spread across `span` pixels: minified by base_extent/span. The matching mip
    // is log2 of that ratio; ratio <= 1 (magnified) wants mip 0 (full detail).
    const float ratio = static_cast<float>(base_extent) / span;
    std::uint32_t base_mip = ratio <= 1.0f ? 0u : static_cast<std::uint32_t>(std::floor(std::log2(ratio)));
    base_mip = std::min(base_mip, levels - 1);
    return std::max(levels - base_mip, coarse_detail);
}

// Decode one texture source (file or embedded bytes) to RGBA8. Pure CPU work — safe to run on a
// job thread. Throws on failure (captured by the job's future, rethrown at .get()).
DecodedTexture decode_texture(const GltfTexture& source)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = source.file.empty()
        ? stbi_load_from_memory(source.encoded.data(), static_cast<int>(source.encoded.size()),
                                &width, &height, &channels, STBI_rgb_alpha)
        : stbi_load(source.file.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("gltf: failed to decode texture " +
            (source.file.empty() ? std::string("<embedded>") : source.file.string()));
    }

    DecodedTexture decoded;
    decoded.pixels.reset(pixels);
    decoded.width = width;
    decoded.height = height;
    decoded.format = source.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return decoded;
}

// Records the (already decoded) texture's upload into a device-local image and binds it into the
// bindless table. Stays on the main thread — allocation + command recording are single-threaded.
void upload_decoded(string::gpu::resource_allocator& allocator,
                    string::gpu::descriptor_table& descriptor_table, TransferBatch& transfer,
                    const DecodedTexture& decoded,
                    string::gpu::resource_id& out_image, uint32_t& out_slot)
{
    // Full mip chain: floor(log2(max dimension)) + 1 levels. TRANSFER_SRC is needed too because
    // mip generation blits from each level down to the next.
    const uint32_t max_dim = static_cast<uint32_t>(std::max(decoded.width, decoded.height));
    const uint32_t mip_levels = static_cast<uint32_t>(std::floor(std::log2(max_dim))) + 1;

    out_image = allocator.create_resource(string::gpu::image_info{
        .extent = { static_cast<uint32_t>(decoded.width), static_cast<uint32_t>(decoded.height), 1 },
        .format = decoded.format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = mip_levels,
    });
    // upload_image copies the pixels into staging immediately; the batch streams it to the GPU
    // asynchronously (bounded by its staging budget), so no per-texture stall.
    const VkDeviceSize size = static_cast<VkDeviceSize>(decoded.width) * decoded.height * 4;
    transfer.upload_image(decoded.pixels.get(), size, out_image);

    descriptor_table.bind(out_image, string::gpu::descriptor_type::TEXTURE);
    out_slot = descriptor_table.get_binding_slot(out_image, string::gpu::descriptor_type::TEXTURE);
}

// The cooked `.ktx2` sibling of an external texture source, if one exists on disk. Embedded
// textures (no file path) have no sibling and always take the stb path. This lets cooking be
// incremental: a texture with a sibling loads via the fast KTX path, the rest fall back to stb.
std::optional<std::filesystem::path> ktx_sibling(const GltfTexture& source)
{
    if (source.file.empty())
    {
        return std::nullopt;
    }
    std::filesystem::path candidate = source.file;
    candidate.replace_extension(".ktx2");
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec))
    {
        return candidate;
    }
    return std::nullopt;
}

// Is the world-space AABB inside the frustum? Gribb-Hartmann planes from the view-projection
// (ZERO_TO_ONE clip) on the CPU, for geometry-residency feedback — we want a draw's geometry as
// soon as it's potentially visible.
bool aabb_in_frustum(const glm::mat4& vp, const glm::vec3& mn, const glm::vec3& mx)
{
    const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    const glm::vec4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
    for (int i = 0; i < 6; ++i)
    {
        const glm::vec3 n(planes[i]);
        const glm::vec3 p(n.x >= 0.0f ? mx.x : mn.x, n.y >= 0.0f ? mx.y : mn.y, n.z >= 0.0f ? mx.z : mn.z);
        if (glm::dot(n, p) + planes[i].w < 0.0f)
        {
            return false;
        }
    }
    return true;
}

// Pack a tangent + handedness into the 10:10:10:2 vertex layout meshlet_mesh.slang unpacks.
uint32_t pack_tangent(const glm::vec3& t, float sign)
{
    const auto sn = [](float v) {
        return static_cast<uint32_t>(static_cast<int32_t>(std::round(glm::clamp(v, -1.0f, 1.0f) * 511.0f))) & 0x3FFu;
    };
    const uint32_t w = sign < 0.0f ? 3u : 1u;   // signed 2-bit: 1 -> +1, 3 (-1 as 2-bit) -> -1
    return sn(t.x) | (sn(t.y) << 10) | (sn(t.z) << 20) | (w << 30);
}

// Brief 07: the standing material-probe (lookdev) scene, baked through the same cook library as
// real content so the whole meshlet path is exercised. A roughness x metallic sphere grid (cols =
// perceptual roughness 0..1, rows = metallic 0..1), a white/mirror pair on the ground in front,
// and a neutral ground slab (shadow catcher). Vertices are pre-transformed (identity draw
// transforms) so DrawInfo.center stays world-space, matching the cooked-scene convention.
assetbake::LoadedScene build_lookdev_scene()
{
    std::vector<String::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    std::vector<GltfMaterial> materials;

    // Unit-sphere template (UV sphere; normal = position, tangent along +phi).
    constexpr int kStacks = 24, kSlices = 48;
    std::vector<String::Vertex> sphere_verts;
    std::vector<uint32_t> sphere_indices;
    for (int st = 0; st <= kStacks; ++st)
    {
        const float theta = glm::pi<float>() * float(st) / float(kStacks);
        for (int sl = 0; sl <= kSlices; ++sl)
        {
            const float phi = 2.0f * glm::pi<float>() * float(sl) / float(kSlices);
            const glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi));
            const glm::vec3 t(-std::sin(phi), 0.0f, std::cos(phi));
            sphere_verts.push_back(String::Vertex{ n, glm::vec3(1.0f),
                { float(sl) / kSlices, float(st) / kStacks }, n, pack_tangent(t, 1.0f) });
        }
    }
    for (int st = 0; st < kStacks; ++st)
        for (int sl = 0; sl < kSlices; ++sl)
        {
            const uint32_t a = uint32_t(st * (kSlices + 1) + sl);
            const uint32_t b = a + kSlices + 1;
            // CCW when viewed from outside (matches the back-face-cull main pipeline).
            sphere_indices.insert(sphere_indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }

    const auto add_sphere = [&](const glm::vec3& center, float radius, int material) {
        const uint32_t v0 = static_cast<uint32_t>(vertices.size());
        const uint32_t i0 = static_cast<uint32_t>(indices.size());
        for (String::Vertex v : sphere_verts)
        {
            v.pos = v.pos * radius + center;
            vertices.push_back(v);
        }
        for (uint32_t i : sphere_indices) indices.push_back(i + v0);
        GltfDraw d;
        d.index_offset = i0;
        d.index_count = static_cast<uint32_t>(sphere_indices.size());
        d.material = material;
        d.transform = glm::mat4(1.0f);
        d.aabb_min = center - glm::vec3(radius);
        d.aabb_max = center + glm::vec3(radius);
        draws.push_back(d);
    };
    const auto add_material = [&](glm::vec3 albedo, float metallic, float roughness) {
        GltfMaterial m;
        m.base_color_factor = glm::vec4(albedo, 1.0f);
        m.metallic_factor = metallic;
        m.roughness_factor = roughness;
        materials.push_back(m);
        return static_cast<int>(materials.size()) - 1;
    };

    // The grid: 8 roughness columns (0..1, perceptual) x 5 metallic rows (0..1), radius-0.5
    // spheres on a wall in the XY plane. Neutral albedo (brighter for metals so the ladder reads).
    constexpr int kCols = 8, kRows = 5;
    constexpr float kSpacing = 1.4f, kRadius = 0.5f;
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col)
        {
            const float metallic = float(row) / float(kRows - 1);
            const float roughness = float(col) / float(kCols - 1);
            const glm::vec3 albedo = glm::mix(glm::vec3(0.5f), glm::vec3(0.9f), metallic);
            add_sphere(glm::vec3((float(col) - (kCols - 1) * 0.5f) * kSpacing,
                                 1.2f + float(row) * kSpacing, 0.0f),
                       kRadius, add_material(albedo, metallic, roughness));
        }
    // The white/mirror pair, on the ground in front of the grid.
    add_sphere(glm::vec3(-1.0f, 0.62f, 2.2f), 0.6f, add_material(glm::vec3(1.0f), 0.0f, 1.0f));
    add_sphere(glm::vec3(1.0f, 0.62f, 2.2f), 0.6f, add_material(glm::vec3(1.0f), 1.0f, 0.0f));

    // Ground slab (two triangles), neutral 40% grey.
    {
        const int mat = add_material(glm::vec3(0.4f), 0.0f, 0.85f);
        const float s = 24.0f;
        const uint32_t v0 = static_cast<uint32_t>(vertices.size());
        const uint32_t i0 = static_cast<uint32_t>(indices.size());
        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const uint32_t tan = pack_tangent(glm::vec3(1, 0, 0), 1.0f);
        const glm::vec3 corners[4] = { { -s, 0, -s }, { s, 0, -s }, { s, 0, s }, { -s, 0, s } };
        const glm::vec2 uvs[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 4; ++i)
            vertices.push_back(String::Vertex{ corners[i], glm::vec3(1.0f), uvs[i], n, tan });
        const uint32_t quad[6] = { v0, v0 + 2, v0 + 1, v0, v0 + 3, v0 + 2 };   // CCW from +Y
        for (uint32_t i : quad) indices.push_back(i);
        GltfDraw d;
        d.index_offset = i0;
        d.index_count = 6;
        d.material = mat;
        d.transform = glm::mat4(1.0f);
        d.aabb_min = { -s, -0.01f, -s };
        d.aabb_max = { s, 0.01f, s };
        draws.push_back(d);
    }

    const auto t0 = std::chrono::steady_clock::now();
    assetbake::CookedScene cs = assetbake::bake_scene(vertices, indices, draws, assetbake::BakeParams{});

    assetbake::LoadedScene out;
    out.vertices = std::move(cs.vertices);
    out.meshlets = std::move(cs.meshlets);
    out.meshlet_vertices = std::move(cs.meshlet_vertices);
    out.meshlet_triangles = std::move(cs.meshlet_triangles);
    out.total_meshlets = cs.total_meshlets;
    out.materials = std::move(materials);
    for (const assetbake::CookedDraw& cd : cs.draws)
    {
        GpuDrawInfo info{};
        info.center = cd.center;
        info.radius = cd.radius;
        info.lod_count = cd.lod_count;
        info.first_meshlet = cd.first_meshlet;
        info.total_meshlets = cd.total_meshlets;
        for (uint32_t l = 0; l < kMaxLods; ++l) info.lods[l] = cd.lods[l];
        out.draws.push_back(info);
        out.draw_windows.push_back({ cd.vertex_count == 0 ? 0u : cd.vertex_offset, cd.vertex_count });
        GltfDraw meta;
        meta.index_offset = cd.index_offset;
        meta.index_count = cd.index_count;
        meta.material = cd.material;
        meta.transform = cd.transform;
        meta.aabb_min = cd.aabb_min;
        meta.aabb_max = cd.aabb_max;
        out.draws_meta.push_back(meta);
    }
    out.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace

GeometryPass::GeometryPass(engine_context& context, std::vector<std::filesystem::path> model_paths,
                           std::shared_ptr<MeshOverlayStats> overlay_stats, bool lookdev)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, residency_(kTextureBudget, kTextureStreamPerFrame)
, gpu_profiler_ctx_(context.gpu_profiler_ctx)
{
    // frames_in_flight_ + overlay_stats_ moved to the GeometryScene base (brief 11 P2); a base
    // class's members can't sit in the derived init list, so seed them first thing in the body
    // (nothing above uses them).
    frames_in_flight_ = context.frames_in_flight;
    overlay_stats_ = std::move(overlay_stats);
    // Brief 11 step 2: cache the scene MSAA sentinels so the geometry.phase2 sub-pass can declare the
    // same color/depth attachments (it forms the reopened MSAA group that loads phase-1's samples).
    color_target_ = context.color_target;
    depth_target_ = context.depth_target;
    // Brief 11 Phase 2 (M3): the whole geometry pass is CVar-toggleable (r.pass.geometry) through
    // the renderer's per-pass enable/disable. Off -> composite reads the cleared color target.
    // Touch the CVar NOW (init-capture invokes cv_pass_geometry()) so it self-registers at
    // construction — before the STRING_PASS_GEOMETRY env override is applied — not lazily at frame 1.
    enable_predicate = [&geom = cv_pass_geometry()] { return geom.get(); };
    // Brief 04e M3: per-frame transient buffers (worklists, draw_lod) reserve into the
    // renderer's scratch arena; buffers bind lazily in update() after materialization.
    scratch_ = &context.scratch;
    // Brief 16 M1: the resource-virtualization hub, for the registry-owned SceneData ring (below)
    // and, later, the light/stats rings (M3).
    resources = &context.resources;
    // Brief 04b: load the COOKED scenes (bake library did the parse/flatten/MikkTSpace/meshletize
    // offline). Per source glTF: read a fresh cooked file, or cook in-process via the library when
    // missing/stale (WARN + CLI hint), then merge N cooked scenes into one draw set (vertex/index/
    // material/texture/meshlet-heap rebasing). No fastgltf/MikkTSpace/meshopt runs here when the
    // cooked files are present — this is the whole point of the brief.
    const auto load_start = std::chrono::steady_clock::now();
    // Brief 04b M4: chunking budget defaults to kChunkMaxMeshlets (on). STRING_CHUNK overrides it for
    // A/B measurement (STRING_CHUNK=0 selects the unchunked .c0 cooked variant); the value also picks
    // the .c<budget>.cooked file, so both variants must be pre-cooked.
    // Brief 06: CVar-backed (r.chunk.budget; legacy STRING_CHUNK alias). Default = kChunkMaxMeshlets.
    uint32_t chunk_budget = static_cast<uint32_t>(std::max(0, cv_chunk_budget().get()));
    lookdev_ = lookdev;
    // Brief 07: the lookdev scene is generated in-process through the same bake library instead
    // of loading cooked glTF (no textures, factors drive the materials).
    assetbake::LoadedScene loaded = lookdev_
        ? build_lookdev_scene()
        : assetbake::load_cooked_scenes(context.resources_path, model_paths, chunk_budget);

    // Adopt the merged geometry-only tables into the runtime containers (the rest of the pass is
    // unchanged: build_meshlet_gpu fills material/transform onto meshlet_model_.draws, the streamer
    // takes the vertex heap + per-draw windows).
    GltfGeometry geometry;
    geometry.vertices = std::move(loaded.vertices);
    // No CPU index buffer in the cooked path (indices were consumed at cook into meshlets).
    geometry.draws = loaded.draws_meta;
    materials_ = std::move(loaded.materials);
    std::vector<GltfTexture> textures = std::move(loaded.textures);
    meshlet_model_.meshlets = std::move(loaded.meshlets);
    meshlet_model_.meshlet_vertices = std::move(loaded.meshlet_vertices);
    meshlet_model_.meshlet_triangles = std::move(loaded.meshlet_triangles);
    meshlet_model_.draws = std::move(loaded.draws);
    meshlet_model_.total_meshlets = loaded.total_meshlets;
    std::vector<GeometryStreamer::Window> draw_windows;
    draw_windows.reserve(loaded.draw_windows.size());
    for (const auto& w : loaded.draw_windows) draw_windows.push_back({ w.offset, w.count });

    STRING_LOG_INFO("[load] cooked scenes: {} files ({} fresh, {} cooked in-process) in {:.1f} ms",
                    model_paths.size(), loaded.cooked_hits, loaded.cooked_misses, loaded.load_ms);

    // Brief 04 (STRING_TRANSP_TEST=1): inject a synthetic set of alpha-blended quads at STAGGERED
    // depths + lateral offsets so the sorted transparency pass can be verified visually. The quads
    // are baked through the SAME library (bake_scene over a tiny synthetic vertex/index/draw set),
    // then merged into the loaded tables so meshlets/windows/DrawInfo all come from one path.
    if (cv_transp_test().get())
    {
        const int base_mat = static_cast<int>(materials_.size());
        const uint32_t meshlet_base = static_cast<uint32_t>(meshlet_model_.meshlets.size());
        const uint32_t mvert_base = static_cast<uint32_t>(meshlet_model_.meshlet_vertices.size());
        const uint32_t mtri_base = static_cast<uint32_t>(meshlet_model_.meshlet_triangles.size());
        const uint32_t vertex_base = static_cast<uint32_t>(geometry.vertices.size());

        const glm::vec4 colors[5] = {
            { 1.0f, 0.2f, 0.2f, 0.5f }, { 0.2f, 1.0f, 0.2f, 0.5f }, { 0.2f, 0.2f, 1.0f, 0.5f },
            { 1.0f, 1.0f, 0.2f, 0.5f }, { 1.0f, 0.2f, 1.0f, 0.5f },
        };
        std::vector<String::Vertex> qv;
        std::vector<uint32_t> qi;
        std::vector<GltfDraw> qd;
        for (int q = 0; q < 5; ++q)
        {
            GltfMaterial m;
            m.base_color_factor = colors[q];
            m.alpha_mode = GltfAlphaMode::Blend;
            m.double_sided = true;
            materials_.push_back(m);

            const float x = 6.0f - static_cast<float>(q) * 1.0f;
            const float cy = 4.5f;
            const float lateral = (static_cast<float>(q) - 2.0f) * 0.4f;
            const float s = 1.6f;
            const uint32_t v0 = static_cast<uint32_t>(qv.size());
            const glm::vec3 nrm{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 col = glm::vec3(colors[q]);
            const glm::vec3 corners[4] = {
                { x, cy - s, lateral - s }, { x, cy - s, lateral + s },
                { x, cy + s, lateral + s }, { x, cy + s, lateral - s },
            };
            for (const glm::vec3& c : corners)
                qv.push_back(String::Vertex{ c, col, { 0.0f, 0.0f }, nrm, 0u });
            const uint32_t idx0 = static_cast<uint32_t>(qi.size());
            const uint32_t quad[6] = { v0, v0 + 1, v0 + 2, v0, v0 + 2, v0 + 3 };
            for (uint32_t k : quad) qi.push_back(k);

            GltfDraw d;
            d.index_offset = idx0;
            d.index_count = 6;
            d.material = q;   // file-local; the merge below rebases by base_mat (as the loader does)
            d.transform = glm::mat4(1.0f);
            d.aabb_min = { x - 0.01f, cy - s, lateral - s };
            d.aabb_max = { x + 0.01f, cy + s, lateral + s };
            qd.push_back(d);
        }
        // Bake the quads (chunking off) and merge into the loaded tables, rebasing meshlet heaps +
        // vertex windows by the current bases.
        assetbake::CookedScene qs = assetbake::bake_scene(qv, qi, qd, assetbake::BakeParams{});
        geometry.vertices.insert(geometry.vertices.end(), qs.vertices.begin(), qs.vertices.end());
        for (uint32_t v : qs.meshlet_vertices) meshlet_model_.meshlet_vertices.push_back(v + vertex_base);
        meshlet_model_.meshlet_triangles.insert(meshlet_model_.meshlet_triangles.end(),
                                                qs.meshlet_triangles.begin(), qs.meshlet_triangles.end());
        for (GpuMeshlet ml : qs.meshlets)
        {
            ml.vertex_offset += mvert_base;
            ml.triangle_offset += mtri_base;
            meshlet_model_.meshlets.push_back(ml);
        }
        for (const assetbake::CookedDraw& cd : qs.draws)
        {
            GpuDrawInfo info{};
            info.center = cd.center;
            info.radius = cd.radius;
            info.lod_count = cd.lod_count;
            info.first_meshlet = cd.first_meshlet + meshlet_base;
            info.total_meshlets = cd.total_meshlets;
            for (uint32_t l = 0; l < kMaxLods; ++l)
            {
                info.lods[l] = cd.lods[l];
                if (l < cd.lod_count) info.lods[l].meshlet_offset += meshlet_base;
            }
            meshlet_model_.draws.push_back(info);
            draw_windows.push_back({ cd.vertex_count == 0 ? 0u : cd.vertex_offset + vertex_base,
                                     cd.vertex_count });
            GltfDraw meta;
            meta.index_offset = cd.index_offset;
            meta.index_count = cd.index_count;
            meta.material = cd.material >= 0 ? cd.material + base_mat : -1;
            meta.transform = cd.transform;
            meta.aabb_min = cd.aabb_min;
            meta.aabb_max = cd.aabb_max;
            geometry.draws.push_back(meta);
        }
        meshlet_model_.total_meshlets = static_cast<uint32_t>(meshlet_model_.meshlets.size());
        STRING_LOG_INFO("[brief04] STRING_TRANSP_TEST: injected 5 blended quads (baked)");
    }

    draws_ = geometry.draws;

    STRING_LOG_INFO("scene loaded: {} files, {} vertices, {} draws, {} materials, {} textures, {} meshlets",
                    model_paths.size(), geometry.vertices.size(),
                    geometry.draws.size(), materials_.size(), textures.size(), meshlet_model_.total_meshlets);

    // Heap capacities (in elements). The streamer suballocates a SEPARATE range per draw, so shared
    // vertices (instanced primitives) are duplicated — size to the sum of per-draw ranges, not the
    // deduplicated vertex count, or everything can't fit even at 100%. kGeometryResidentPercent < 100
    // caps below that to exercise reclaim (with pop-in / possible thrash when the visible set exceeds
    // the budget); 100 keeps everything resident (reclaim still happens for geometry left behind as
    // you look around, freeing ranges — visible in the [geo] logs — but no artifacts).
    // Sum the per-draw vertex windows (the streamer suballocates one range per draw, so shared
    // instanced vertices are duplicated — size to the window sum, not the deduplicated vertex count).
    // Windows are baked at cook, so no runtime index scan is needed.
    uint64_t vertex_units = 0;
    for (const GeometryStreamer::Window& w : draw_windows) vertex_units += w.count;
    const uint64_t vertex_capacity = std::max<uint64_t>(1, vertex_units * kGeometryResidentPercent / 100);
    const VkDeviceSize vertex_size = VkDeviceSize(sizeof(String::Vertex)) * vertex_capacity;
    // Brief 04 M3/04b: no GPU index heap AND no CPU index buffer — the task/mesh shaders pull vertices
    // via the meshlet-vertex remap heap (baked at cook); the streamer uploads per-draw vertex windows.

    // Model AABB, computed now (before the geometry arrays are moved into the streamer below) for
    // framing the camera.
    glm::vec3 aabb_min(std::numeric_limits<float>::max());
    glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
    for (const auto& vertex : geometry.vertices)
    {
        aabb_min = glm::min(aabb_min, vertex.pos);
        aabb_max = glm::max(aabb_max, vertex.pos);
    }
    if (geometry.vertices.empty())
    {
        aabb_min = glm::vec3(-1.0f);
        aabb_max = glm::vec3(1.0f);
    }

    // Shared geometry buffers allocated whole up front, but NOT uploaded here: the geometry
    // streamer uploads each draw's vertex/index sub-range on demand (when the draw enters view).
    // Vertices are pulled by device address in the vertex shader, so the buffer needs
    // SHADER_DEVICE_ADDRESS (which populates allocated_buffer.device_address) rather than VERTEX_BUFFER.
    vertex_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
               | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // 1x1 white fallback for draws without a base-color texture (the factor still tints it). Created
    // first because the streamer also uses it as the placeholder shown for cooked textures until
    // their deferred transcode+upload lands.
    const std::array<uint8_t, 4> white_pixel = { 255, 255, 255, 255 };
    white_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_image(white_pixel.data(), white_pixel.size(), white_image_);
    descriptor_table_.bind(white_image_, string::gpu::descriptor_type::TEXTURE);
    white_slot_ = descriptor_table_.get_binding_slot(white_image_, string::gpu::descriptor_type::TEXTURE);
    const string::gpu::allocated_image& white = allocator_.get_image(white_image_);

    // 1x1 flat-normal fallback (tangent-space +Z): draws with no normal map sample this and get the
    // geometric normal back, so the fragment shader never branches on "has a normal map".
    const std::array<uint8_t, 4> flat_normal_pixel = { 128, 128, 255, 255 };
    flat_normal_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,   // linear, not sRGB — it's data, not colour
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_image(flat_normal_pixel.data(), flat_normal_pixel.size(), flat_normal_image_);
    descriptor_table_.bind(flat_normal_image_, string::gpu::descriptor_type::TEXTURE);
    flat_normal_slot_ = descriptor_table_.get_binding_slot(flat_normal_image_, string::gpu::descriptor_type::TEXTURE);

    // Texture sources come from the cooked scenes (path + srgb). Cooked .ktx2 siblings register with
    // the streamer; the rest decode via stb on a worker pool. (Textures are unchanged this brief; the
    // cooked format just carries the paths, so the decode/upload path below is the same as before.)
    string::core::job_system decode_pool;
    std::vector<std::optional<std::filesystem::path>> ktx_paths(textures.size());
    std::vector<std::future<DecodedTexture>> decode_jobs(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        ktx_paths[i] = ktx_sibling(textures[i]);
        if (!ktx_paths[i])
        {
            const GltfTexture* source = &textures[i];
            decode_jobs[i] = decode_pool.enqueue([source]() { return decode_texture(*source); });
        }
    }

    // Collect textures: cooked KTX2 register with the streamer (BC7 image + placeholder slot now,
    // mips streamed in on demand from the coarse tail up); stb fallbacks upload whole here.
    texture_streamer_ = std::make_unique<TextureStreamer>(
        device_, allocator_, descriptor_table_, context.transfer, white.view, white.sampler);
    texture_images_.resize(textures.size());
    texture_slots_.resize(textures.size());
    texture_lod_.resize(textures.size());
    frame_desired_detail_.resize(textures.size(), 0);
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        if (ktx_paths[i])
        {
            const TextureStreamer::Registered reg =
                texture_streamer_->add(*ktx_paths[i], textures[i].srgb);
            texture_images_[i] = reg.image;
            texture_slots_[i] = reg.slot;
            residency_.register_resource(reg.image, *texture_streamer_, reg.min_detail,
                                         reg.max_detail, reg.min_detail);
            streamed_textures_.push_back(reg.image);
            texture_lod_[i] = TextureLod{
                .levels = reg.max_detail,
                .base_extent = reg.base_extent,
                .coarse_detail = coarse_detail_for(reg.max_detail, reg.base_extent, kCoarseFloorPixels),
            };
        }
        else
        {
            const DecodedTexture decoded = decode_jobs[i].get();
            upload_decoded(allocator_, descriptor_table_, context.transfer, decoded,
                           texture_images_[i], texture_slots_[i]);
        }
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_start).count();
    STRING_LOG_INFO("[load] total ({} textures, {} decode workers): {} ms",
                    textures.size(), decode_pool.worker_count(), load_ms);
    STRING_LOG_INFO("[stream] {} of {} textures streamed (BC7, coarse tail first); rest stb",
                    texture_streamer_->count(), textures.size());
    // The renderer drains the transfer batch (wait_idle) once after all passes are built; no
    // per-pass flush needed here.

    // --- GPU-driven draw data ------------------------------------------------------------------
    draw_count_ = static_cast<uint32_t>(draws_.size());
    if (draw_count_ > 0)
    {
        // Brief 04b: meshlets + LOD chain were baked offline (loaded above into meshlet_model_).
        // The GPU meshlet buffers are uploaded in build_meshlet_gpu() below; the DrawInfo table's
        // material/transform fields are filled there too (the cooked draws are geometry-only records).
        STRING_LOG_INFO("[meshlet] {} draws -> {} meshlets, {} vtx-remap, {} tri-words (cooked)",
                        meshlet_model_.draws.size(), meshlet_model_.total_meshlets,
                        meshlet_model_.meshlet_vertices.size(), meshlet_model_.meshlet_triangles.size());

        // Geometry streaming: hand the shared vertex heap to the streamer (suballocates + uploads
        // per-draw windows on demand, frees on eviction) and register every draw with the geometry
        // residency manager. The manager's byte budget matches the heap capacity so it evicts once
        // full. On residency the streamer flips the draw's DrawInfo gate (resident + vertex_offset).
        // No CPU index buffer now (cooked path): windows come from the baked per-draw vertex ranges.
        geometry_streamer_ = std::make_unique<GeometryStreamer>(
            allocator_, context.transfer, vertex_buffer_,
            std::move(geometry.vertices), vertex_capacity, frames_in_flight_);
        geometry_streamer_->set_windows(draw_windows);
        geometry_streamer_->set_residency_callback(
            [this](std::uint32_t draw, std::uint32_t vertex_offset, std::uint32_t resident) {
                // Streaming seam: DrawInfo.resident is the ONLY draw gate. vertex_offset (heap delta)
                // rides along — meshlet-vertices hold ORIGINAL global indices and the mesh shaders
                // rebase them into the suballocated heap.
                if (draw_info_mapped_)
                {
                    draw_info_mapped_[draw].resident = resident;
                    draw_info_mapped_[draw].vertex_offset = vertex_offset;
                    // Brief 04d: a residency change makes this draw's meshlets "new" — their persistent
                    // visibility bits are stale. Flag a bitfield clear so every meshlet re-validates via
                    // phase 2 next frame (a cleared bit is correct: one phase-2 frame, no popping). The
                    // clear is a cheap fill of a small buffer; flagging (not clearing per-range here from
                    // the CPU — the bitfield is device-local) keeps the streaming callback trivial.
                    visbits_clear_pending_ = true;
                }
            });

        // Build the GPU meshlet buffers + the DrawInfo table (host-visible resident gate) now, so the
        // up-front streaming loop below can flip DrawInfo.resident via the callback.
        build_meshlet_gpu(context);

        geometry_streaming_ = kGeometryResidentPercent < 100;
        if (geometry_streaming_)
        {
            // Doesn't fit: register with the manager and stream per-frame by visibility (update()).
            // Brief 04 M3: the budget is vertex-heap only now (the GPU index heap is gone), matching
            // the streamer's vertex-only cost() so eviction decisions stay coherent with what's uploaded.
            const VkDeviceSize geometry_budget = VkDeviceSize(vertex_capacity) * sizeof(String::Vertex);
            geometry_residency_ = std::make_unique<string::gpu::residency_manager>(
                geometry_budget, kGeometryStreamPerFrame);
            for (std::uint32_t d = 0; d < draw_count_; ++d)
            {
                geometry_residency_->register_resource(d, *geometry_streamer_, /*min=*/0, /*max=*/1, /*initial=*/0);
            }
        }
        else
        {
            // Fits: upload every draw's geometry now (drained once by the renderer's wait_idle before
            // frame 0) and reveal it. No per-frame streaming/eviction — same clean up-front load as
            // before geometry streaming existed.
            for (std::uint32_t d = 0; d < draw_count_; ++d)
            {
                geometry_streamer_->stream(d, 0, 1);
                geometry_streamer_->on_resident(d, 1);
            }
        }
    }

    // Frame the whole model with the engine camera using the AABB computed above, then let it
    // position itself to fit. Bind the conventional fly controls (WASD + Space/Ctrl + Shift) onto
    // the shared InputMap so update() can read them by action name.
    String::Camera::bind_default_controls(input_map_);
    // Debug: F freezes/unfreezes the culling frustum, C toggles culling off entirely (see update()).
    input_map_.bind_button("freeze_culling", String::KeyCode::F);
    input_map_.bind_button("toggle_culling", String::KeyCode::C);
    // Brief 03 debug keys: O toggles HiZ occlusion, V cycles debug views
    // (none/meshlet/LOD/occlusion-reject), K spawns the crowd stress scene, G toggles LOD select.
    input_map_.bind_button("toggle_hiz", String::KeyCode::O);
    input_map_.bind_button("cycle_debug_view", String::KeyCode::V);
    input_map_.bind_button("toggle_crowd", String::KeyCode::K);
    input_map_.bind_button("toggle_lod", String::KeyCode::G);
    camera_.frame_bounds(aabb_min, aabb_max);

    // Brief 06: the improvised STRING_* levers are now CVar-backed (legacy env names kept as aliases,
    // see debug_cvars.*). Seed the runtime-mutable toggles from the CVars; the F/C/O/L/V/G/K keys
    // still flip the members live in update(). Recipes like STRING_HIZ=0 / STRING_CAM=... work verbatim.
    hiz_enabled_ = cv_hiz_enabled().get();
    lod_enabled_ = cv_lod_enabled().get();
    cull_enabled_ = cv_cull_enabled().get();
    lights_enabled_ = cv_lights_enabled().get();
    debug_view_ = cv_debug_view().get();
    // STRING_CAM="px,py,pz,yaw,pitch" (radians); empty CVar = keep the scene-framed default pose.
    if (const std::string cam = cv_camera_pose().get(); !cam.empty())
    {
        glm::vec3 pos{}; float yaw = 0.0f, pitch = 0.0f;
        if (std::sscanf(cam.c_str(), "%f,%f,%f,%f,%f", &pos.x, &pos.y, &pos.z, &yaw, &pitch) == 5)
            camera_.set_pose(pos, yaw, pitch);
    }
    // r.crowd.enabled (STRING_CROWD) triggers the K-toggle crowd stress path at first update
    // (headless benchmark). The build is deferred to update() (needs residency known).
    crowd_enabled_ = cv_crowd_enabled().get();
    // Brief 07: headless TOD sequence lever (the T key toggle, pre-armed).
    sun_animate_ = cv_sun_animate().get();
    // The lookdev probe scene reads material response under sun + sky IBL only — the local-light
    // stress set would pollute it (L / STRING_LIGHTS=1 still re-enable it explicitly).
    if (lookdev_ && std::getenv("STRING_LIGHTS") == nullptr) lights_enabled_ = false;
    // (STRING_DRAW_MIN/MAX bisection removed with brief 03b: draws are now GPU-generated into one
    //  indirect list, so a CPU draw-index window no longer maps to the dispatch loop.)

    // Declared graph usages: the shared buffers + the render targets. The uploaded textures are
    // static SHADER_READ_ONLY inputs (transitioned once by the transfer batch), so they're not
    // graph-tracked and don't need declaring here.
    usages = {
        { vertex_buffer_,       Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        // EARLY|LATE: the MSAA depth STORE (brief 04d two-phase chain: phase-1 group stores depth for
        // phase-2 to reload) completes at LATE_FRAGMENT_TESTS, so the tracker must record that stage —
        // else the phase-2 group's reload transition's src scope misses it (sync-validation WAW).
        { context.depth_target, Access::DepthWrite,
          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT },
    };
    // Brief 04e M2: buffer state IS graph-tracked now. The static usages above are the fixed
    // prefix; update() re-appends the per-frame-slot buffer usages (worklists, froxel lists, the
    // visibility bitfield) each frame, and the renderer derives the compute->draw and cross-frame
    // barriers from them (the old renderer-side broad barrier is deleted).
    static_usage_count_ = usages.size();

    // Procedural sky background is now the standalone SkyPass (brief 11 step 3).

    // --- Cascaded shadow maps + Forward+ scene/light/froxel buffers ---------------------------
    if (draw_count_ > 0)
    {
        scene_aabb_min_ = glm::vec3(std::numeric_limits<float>::max());
        scene_aabb_max_ = glm::vec3(std::numeric_limits<float>::lowest());
        for (const GltfDraw& d : draws_)
        {
            scene_aabb_min_ = glm::min(scene_aabb_min_, d.aabb_min);
            scene_aabb_max_ = glm::max(scene_aabb_max_, d.aabb_max);
        }

        // Nearest + clamp sampler: manual PCF compares raw depth samples, so no linear filtering.
        const VkSamplerCreateInfo shadow_sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        };
        if (vkCreateSampler(device_.get_device(), &shadow_sampler_info, nullptr, &shadow_sampler_) != VK_SUCCESS)
        {
            throw std::runtime_error("GeometryPass: failed to create shadow sampler");
        }

        // One D32 shadow image per cascade per frame in flight (frame N+1's render must not race N's
        // sample, and each cascade has its own map). Extra (unused) cascade slots are left null.
        shadow_images_.resize(frames_in_flight_);
        shadow_slots_.resize(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            for (uint32_t c = 0; c < settings_.cascade_count; ++c)
            {
                const string::gpu::resource_id img = allocator_.create_resource(string::gpu::image_info{
                    .extent = { settings_.shadow_resolution, settings_.shadow_resolution, 1 },
                    .format = VK_FORMAT_D32_SFLOAT,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                    .allocation_flags = {},
                });
                descriptor_table_.bind(img, string::gpu::descriptor_type::TEXTURE);
                const uint32_t slot = descriptor_table_.get_binding_slot(img, string::gpu::descriptor_type::TEXTURE);
                descriptor_table_.update_texture(slot, allocator_.get_image(img).view, shadow_sampler_);
                shadow_images_[f][c] = img;
                shadow_slots_[f][c] = slot;
            }
        }

        // --- Per-frame SceneData SSBO ring (device-addressed, persistent-mapped) ---
        // Brief 16 M1: a registry-owned PerFrame buffer (was a hand-managed [frame] ring of
        // resource_ids + mapped pointers). The registry allocates one physical per frame-in-flight
        // and owns them; resolve address/mapped by slot via resources->{address,mapped}(scene_buffer_, f).
        scene_buffer_ = resources->create_per_frame(string::gpu::buffer_info{
            .size = sizeof(SceneData),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        }, frames_in_flight_);

        // --- Local lights: build the stress scene + per-frame light SSBO ring ---
        build_light_stress_scene(scene_aabb_min_, scene_aabb_max_);
        const VkDeviceSize light_capacity = sizeof(GpuLight) * std::max<std::size_t>(1, lights_.size());
        light_buffer_ = resources->create_per_frame(string::gpu::buffer_info{
            .size = light_capacity,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        }, frames_in_flight_);

        // Froxel light-binning + dynamic sky IBL are their own passes now (brief 11 step 3).

        // Brief 09b: relightable irradiance probe volume (grid fit to the scene AABB + atlases +
        // relight/capture/debug pipelines). Fits the scene AABB computed just above.
        create_probe_resources(context);

        // Brief 09: GTAO pipelines + sampler (the half-res targets are screen-sized — created
        // lazily in ensure_gtao once the extent is known). Linear clamp: the lit shader bilinearly
        // upsamples the half-res AO.
        {
            const VkSamplerCreateInfo gtao_sampler_info = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            };
            if (vkCreateSampler(device_.get_device(), &gtao_sampler_info, nullptr, &gtao_sampler_)
                != VK_SUCCESS)
                throw std::runtime_error("GeometryPass: failed to create GTAO sampler");
            VkDescriptorSetLayout gtao_layout = descriptor_table_.get_layout();
            const auto make_gtao_entry = [&](const char* entry) {
                return context.shader_registry.create(
                    context.resources_path / "shaders" / "gtao.slang",
                    [gtao_layout, entry](string::gpu::device& dev,
                                         const string::gpu::compiled_program& compiled) {
                        string::gpu::pipeline p{};
                        p.push_constants = compiled.layout.push_constant;
                        p.pipeline_layout = string::gpu::pipeline_layout_builder()
                            .set_descriptor_set_layout({ gtao_layout })
                            .set_push_constant_ranges({ compiled.layout.push_constant })
                            .build(dev);
                        string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::COMPUTE);
                        for (const auto& stage : compiled.stages)
                            if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && stage.entry_point == entry)
                                builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                        p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                        p.pipeline_type = string::gpu::pipeline_type::COMPUTE;
                        return p;
                    });
            };
            gtao_program_ = make_gtao_entry("gtao_main");
            gtao_denoise_program_ = make_gtao_entry("gtao_denoise_main");
            hz_depth_valid_.assign(frames_in_flight_, 0);
            gtao_slot_view_.assign(frames_in_flight_, glm::mat4(1.0f));
            gtao_slot_proj_.assign(frames_in_flight_, glm::mat4(1.0f));
            gtao_slot_view_proj_.assign(frames_in_flight_, glm::mat4(1.0f));
        }

        // Debug controls: T animate sun (time-of-day), [ / ] scrub it, L toggle local lights,
        // H toggle the froxel heatmap.
        input_map_.bind_button("sun_animate", String::KeyCode::T);
        input_map_.bind_button("time_back", String::KeyCode::LEFT_BRACKET);
        input_map_.bind_button("time_fwd", String::KeyCode::RIGHT_BRACKET);
        input_map_.bind_button("toggle_lights", String::KeyCode::L);
        input_map_.bind_button("toggle_heatmap", String::KeyCode::H);
    }
}

// --- Brief 07: dynamic sky IBL --------------------------------------------------------------------
// Two small RGBA16F cubemaps (capture chain + prefiltered roughness ladder), the 9-coefficient SH
// buffer and the split-sum DFG LUT, plus the five compute pipelines that fill them (ibl.slang).
// The cubemaps live in GENERAL layout for their whole life (compute writes + sampled reads both
// legal there; 128px — layout-optimal compression is irrelevant), which keeps the intra-pass sync
// to plain memory barriers. The DFG LUT is baked once and parked in SHADER_READ_ONLY.

// Record the sky-IBL update chain: capture -> capture mip chain -> SH projection + GGX prefilter
// ladder. Runs only on frames where the sun moved past the trigger (see update()) — the whole
// chain is a single-frame update, so the ambient is always self-consistent (no popping). The DFG
// LUT bake rides the first call. All barriers here are the documented INTRA-pass class (like the
// HiZ mip chain): everything is produced and consumed by this pass; the SH buffer's fragment-read
// edge is graph-declared (usages) and the final memory barrier makes the image writes visible to
// the fragment stage.



// dbg.ibl_verify (brief 07 M1 numeric gate): read the DFG LUT + SH coefficients back and check
// them against references. PASS criteria: (a) DFG matches the CPU double-precision integral at
// probe points within 0.02 and A+B stays in (0, 1.01] (single-scatter albedo can't exceed 1);
// (b) under r.furnace the SH DC reconstructs E/pi = 1 +- 0.02 with all higher bands ~0; without
// the furnace, reconstructed sky irradiance is finite and up > down (sky brighter than ground).

// Upload the meshlet heaps, build the DrawInfo table (materials + transforms + bounds + LOD ranges),
// allocate the visibility bitfield + stats ring, and create the task/mesh/HiZ/reset pipelines.

// Build (or tear down) the crowd stress scene: duplicate every base draw across a kCrowdGrid x
// kCrowdGrid grid of translated copies, to prove 500+-crowd geometry throughput (brief M6). Crowd
// draws are extra DrawInfo entries referencing the SAME meshlet buffers — only the transform (and
// thus the world-space bounds) differ. Visibility bits are shared with the base draws (conservative
// for the shared meshlet ids — fine for a throughput stress test).

// (Re)create the HiZ pyramid for the current screen size if it changed. Power-of-two conservative
// sizing (each mip is half, rounded down, min 1); R32F mip chain. Binds the whole-chain sampled slot
// (binding 1) + a storage-image slot per mip (binding 2) for the downsample compute.

// --- Brief 09: GTAO targets ------------------------------------------------------------------------
// Half-res RGBA8 (rgb = world bent normal, a = visibility): one shared raw target (produced and
// denoised within one record hook) + one final target per frame slot (this frame's fragments
// sample it while the next frame's chain rewrites its own slot).

// Record the GTAO chain (top of record_compute): horizon-search AO + bent normal from the
// PREVIOUS slot's resolved depth, then the spatial denoise into this slot's final target.
// Barriers are the documented cross-frame local class (07 precedent: same-queue, cross-CB;
// hz.depth is untracked pass-managed state, its resolve re-discards from UNDEFINED).

// Brief 03b: dispatch the GPU draw-cull compute to build a work list (commands[] + records[] + count)
// for one consumer. `prepass` forces LOD0 (and skips the LOD histogram) for the HiZ depth prepass;
// the camera list (prepass=false) selects LODs and writes the histogram, and is reused by the main
// pass + all 3 shadow cascades. The list buffer's count is zeroed here, then the compute appends.
// Leaves a barrier making commands[]/records[]/count visible to DRAW_INDIRECT + task-shader reads.

// Brief 04c COMPACTION PHASE for one worklist: scan_blocks -> scan_carry -> fill, then a barrier so the
// resulting commands[] + records[] + count are visible to the task shader / indirect draw. The three
// passes are separated by storage barriers (each reads the previous pass's writes). The fill copies the
// per-draw selected LOD from `draw_lod` into each surviving draw's record.

// Brief 04: build the sorted transparency work list on the CPU. BLEND draws (excluded from the opaque
// GPU lists) are sorted BACK-TO-FRONT by their world-space bounding-sphere distance to the eye, then
// written as a compacted commands[] + records[] + count into the host-visible per-frame buffer. v1
// uses LOD0 for every transparent draw (counts are small; per-draw sort only — no per-meshlet/OIT).
// Returns the number of draws written (indirect count). Runs on the render thread (host-visible write).

// Brief 04: draw the sorted transparency list (after opaque + sky). Same push as the main pass but
// blend_pass=1 (fragment outputs base-color alpha) via the transparency pipeline (blend on, depth
// test vs opaque depth, no depth write). HiZ stays valid (occlusion vs opaque depth is fine).

// Brief 04c (resolution b): the main pass draws the compacted camera list with ONE
// vkCmdDrawMeshTasksIndirectCountEXT. The compaction phase wrote the DENSE commands[] + records[] +
// count into `wl` (one command per surviving draw, ascending draw order); the task shader reads its
// {draw_index, lod} via SV_DrawIndex. groupCountX per command = ceil(LOD meshlets / 32). Command-index
// ordering across the draws is what pins coplanar depth-tie winners run-to-run (AE=0).

// Brief 04e M4: froxel light binning — one thread per froxel bins the local lights into
// per-froxel index lists the lit fragment shader reads. DEPENDENCY-FREE within the frame (reads
// only the host-written light SSBO ring), so the renderer places it on an async compute lane
// when the hardware exposes one (timeline edge + queue-family ownership transfer derived from
// async_usages), or records it inline on the main queue otherwise. No Tracy zone here: the
// renderer wraps the whole async chain in the LANE's own GPU context (a pass-side zone would
// use the main-queue context and produce bogus timestamps on the async queue).


// Time-of-day 0..1 -> a sun direction arc (east low -> zenith -> west low) and the sky/sun palette.
namespace
{
struct SunState { glm::vec3 dir; glm::vec3 sun_color; float sun_intensity; glm::vec3 sky_zenith; glm::vec3 sky_ground; };
SunState sun_for_time(float t)
{
    // Great-circle day path: the sun rides a single TILTED CIRCLE from the east horizon, arcing up
    // and leaning south, down to the west horizon — a natural wide arc (the old model computed a
    // half-sine elevation and a linear azimuth INDEPENDENTLY, which isn't a circle and read as a
    // steep "^" tent because the azimuth barely swept). `lean` (r.sun.lean) tilts the arc toward
    // south: smaller = higher noon sun (~0 = straight overhead), larger = a lower, flatter arc.
    const float p = t * glm::pi<float>();               // 0 (east horizon) .. pi (west horizon)
    const float lean = cv_sun_lean().get();
    glm::vec3 d(-std::cos(p),                            // east -> west
                std::sin(p) * std::cos(lean),            // up (arcs over the day)
                std::sin(p) * std::sin(lean));           // south lean
    d.y = d.y * 0.94f + 0.06f;                           // keep the "never fully below horizon" floor
    SunState s;
    s.dir = glm::normalize(d);
    // Warm at the horizon (sunrise/sunset), neutral-bright at noon. Drive off the actual elevation.
    const float noon = glm::clamp(s.dir.y, 0.0f, 1.0f);
    s.sun_color = glm::mix(glm::vec3(1.0f, 0.55f, 0.28f), glm::vec3(1.0f, 0.96f, 0.9f), noon);
    // Brief 07 M4 units (self-consistent, "physical-ish"): illuminance in KILOLUX, luminance /
    // radiance in KILO-NITS (1 unit = 1000 lx / 1000 cd/m^2 — see r.exposure.ev100 for the
    // matching EV100 exposure). Sun: ~100 klx perpendicular at noon, ~7 klx at the horizon.
    // Sky radiance: clear-day zenith ~6 knits at noon falling toward dusk (consistency: pi * mean
    // sky radiance ~= 15-25 klx of diffuse skylight, the right fraction of the 100 klx global).
    s.sun_intensity = glm::mix(7.0f, 100.0f, noon);
    s.sky_zenith = glm::mix(glm::vec3(0.24f, 0.40f, 0.96f), glm::vec3(2.8f, 6.0f, 12.4f), noon);
    // Ground band is a constant ALBEDO; its radiance is derived per-direction in the shader from
    // the CURRENT sun + sky (sky_ground_radiance in sky.slang), so it dims/warms with time of day
    // instead of radiating noon-warm at dusk. Calibrated to reproduce the old noon ground
    // radiance (2.64, 2.28, 1.80 knits) at t=0.5.
    s.sky_ground = glm::vec3(0.0824f, 0.0699f, 0.0503f);
    return s;
}
}  // namespace



// ensure_froxel_capacity moved to FroxelComponent::ensure_capacity (brief 11).

GeometryPass::~GeometryPass()
{
    auto destroy_program = [this](string::gpu::shader_program* prog) {
        if (!prog) return;
        const string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    // sky + froxel + IBL programs torn down in their own passes' destructors now (brief 11 step 3).
    destroy_program(meshlet_program_);
    destroy_program(meshlet_twosided_program_);
    destroy_program(meshlet_transparent_program_);
    destroy_program(meshlet_shadow_program_);
    destroy_program(hiz_program_);
    destroy_program(gtao_program_);
    destroy_program(gtao_denoise_program_);
    destroy_program(reset_program_);
    destroy_program(draw_cull_program_);
    destroy_program(expand_scan_blocks_program_);
    destroy_program(expand_scan_carry_program_);
    destroy_program(expand_fill_program_);
    // Brief 09b probe GI programs.
    destroy_program(probe_clear_program_);
    destroy_program(probe_collapse_program_);
    destroy_program(probe_relight_program_);
    destroy_program(probe_capture_raster_program_);
    destroy_program(probe_debug_program_);

    // Brief 03 meshlet resources.
    if (meshlet_model_.total_meshlets > 0)
    {
        for (HizPyramid& hz : hiz_)
        {
            for (uint32_t s : hz.mip_storage_slots) descriptor_table_.unbind_storage_view(s);
            for (VkImageView v : hz.mip_views) vkDestroyImageView(device_.get_device(), v, nullptr);
            // hz.image / hz.depth are registry-owned PerFrame image rings now (brief 16 M7 #1) — the
            // registry frees them; here we only release this slot's bindless descriptor + views.
            if (hz.image != 0) descriptor_table_.unbind(hz.image, string::gpu::descriptor_type::TEXTURE);
            if (hz.depth != 0) descriptor_table_.unbind(hz.depth, string::gpu::descriptor_type::TEXTURE);
        }
        if (hiz_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_.get_device(), hiz_sampler_, nullptr);
        // Brief 09 GTAO targets.
        // gtao raw/final image rings are registry-owned now (brief 16 M7 #1) — the registry frees the
        // physicals; here we only release this pass's bindless slots (sampled + storage views).
        if (gtao_raw_storage_slot_ != UINT32_MAX)
            descriptor_table_.unbind_storage_view(gtao_raw_storage_slot_);
        for (uint32_t s : gtao_final_storage_slots_) descriptor_table_.unbind_storage_view(s);
        if (gtao_raw_ != 0) descriptor_table_.unbind(gtao_raw_, string::gpu::descriptor_type::TEXTURE);
        for (string::gpu::resource_id id : gtao_final_)
            if (id != 0) descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
        // stats_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M3).
        // Brief 04e M3: worklists + draw_lod live in the renderer-owned scratch arena — nothing
        // to free here.
        for (const string::gpu::resource_id b : transp_buffers_) if (b) allocator_.destroy_resource(b);
        allocator_.destroy_resource(draw_info_buffer_);
        allocator_.destroy_resource(meshlet_triangles_);
        allocator_.destroy_resource(meshlet_vertices_);
        allocator_.destroy_resource(meshlet_buffer_);
    }

    if (!shadow_images_.empty())
    {
        vkDestroySampler(device_.get_device(), shadow_sampler_, nullptr);
        for (const auto& per_frame : shadow_images_)
        {
            for (uint32_t c = 0; c < settings_.cascade_count; ++c)
            {
                descriptor_table_.unbind(per_frame[c], string::gpu::descriptor_type::TEXTURE);
                allocator_.destroy_resource(per_frame[c]);
            }
        }
        // scene_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M1).
        // light_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M3).
        // froxel index buffers freed in FroxelPass's destructor now (brief 11 step 3).
    }

    // Brief 07 IBL resources torn down in IblPass's destructor now (brief 11 step 3).
    // Brief 09/09b: GTAO sampler + probe volume were created in the same ctor block as the IBL
    // resources; their own inner guards make the former `env_capture_ != 0` gate unnecessary.
    {
        // Brief 09: the GTAO sampler is created alongside the IBL resources.
        if (gtao_sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device_.get_device(), gtao_sampler_, nullptr);
        // Brief 09b: probe volume atlases + sampler.
        if (probe_volume_.valid)
        {
            for (string::gpu::resource_id id : { probe_irrad_, probe_cap_gbuf_, probe_cap_albedo_, probe_vis_ })
            {
                if (id != 0)
                {
                    descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
                    allocator_.destroy_resource(id);
                }
            }
            // Cube G-buffer: 6-layer array views, cube images (albedo/nd sampled), classification +
            // relocation buffers, meshlet->draw table.
            for (VkImageView v : { probe_cube_albedo_array_view_, probe_cube_nd_array_view_,
                                   probe_cube_depth_array_view_ })
                if (v) vkDestroyImageView(device_.get_device(), v, nullptr);
            for (string::gpu::resource_id id : { probe_cube_albedo_, probe_cube_nd_ })
                if (id != 0)
                {
                    descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
                    allocator_.destroy_resource(id);
                }
            if (probe_cube_depth_ != 0) allocator_.destroy_resource(probe_cube_depth_);
            if (probe_active_ != 0) allocator_.destroy_resource(probe_active_);
            if (probe_offset_ != 0) allocator_.destroy_resource(probe_offset_);
            if (probe_meshlet_draw_ != 0) allocator_.destroy_resource(probe_meshlet_draw_);
            if (probe_sampler_ != VK_NULL_HANDLE)
                vkDestroySampler(device_.get_device(), probe_sampler_, nullptr);
        }
    }

    descriptor_table_.unbind(white_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(white_image_);
    descriptor_table_.unbind(flat_normal_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(flat_normal_image_);
    for (const string::gpu::resource_id image : texture_images_)
    {
        descriptor_table_.unbind(image, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(image);
    }

    allocator_.destroy_resource(vertex_buffer_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
{
    // Brief 04e M3: bind each frame slot's scratch buffer into the worklist handles once the
    // renderer has materialized the arena (post-construction, pre-first-frame).
    if (!scratch_bound_ && scratch_ != nullptr && scratch_->materialized())
    {
        for (uint32_t f = 0; f < wl_opaque_.size(); ++f)
        {
            const string::gpu::resource_id buf = scratch_->buffer(f);
            wl_opaque_[f].buffer = buf;
            wl_twosided_[f].buffer = buf;
            for (Worklist& wl : wl_shadow_[f]) wl.buffer = buf;
        }
        scratch_bound_ = true;
    }
    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);
    camera_.update(input_map_, delta_time, aspect);

    // dbg.orbit (STRING_ORBIT): continuously sway the camera around a latched base pose so headless
    // captures exercise per-frame disocclusion — the two-phase phase-1/phase-2 interleaved path that
    // no static capture reaches. Small radius + yaw/pitch sweep = enough new pixels each frame to make
    // phase 2 non-empty. Applied AFTER camera_.update so it fully overrides the (idle) input path.
    if (const float orbit_speed = cv_orbit().get(); orbit_speed != 0.0f)
    {
        if (!orbit_base_latched_)
        {
            orbit_base_pos_ = camera_.position();
            orbit_base_yaw_ = camera_.yaw();
            orbit_base_pitch_ = camera_.pitch();
            orbit_base_latched_ = true;
        }
        orbit_phase_ += orbit_speed * delta_time;
        // Small lateral/vertical circle around the base position + a coupled yaw/pitch sweep so both
        // the eye point AND the view direction change every frame (maximal disocclusion, no degenerate
        // pure-roll where the depth buffer barely changes).
        const float radius = 0.35f;              // world units — small, enough for per-frame reveal
        const float yaw_amp = 0.06f;             // radians (~3.4 deg) view sweep
        const float pitch_amp = 0.03f;
        const glm::vec3 pos = orbit_base_pos_
            + glm::vec3(std::cos(orbit_phase_) * radius, std::sin(orbit_phase_ * 0.5f) * radius * 0.4f,
                        std::sin(orbit_phase_) * radius);
        camera_.set_pose(pos,
                         orbit_base_yaw_ + std::sin(orbit_phase_) * yaw_amp,
                         orbit_base_pitch_ + std::sin(orbit_phase_ * 0.7f) * pitch_amp);
        // Re-run the matrix build with the swayed pose (update() built view_proj from the pre-sway
        // pose). A zero-delta update with no input just rebuilds the matrices from the members.
        camera_.update(input_map_, 0.0f, aspect);
    }

    // Debug GPU readback (STRING_MESHLET_READBACK=1): memcmp GPU meshlet buffers vs CPU arrays
    // well after the load-time uploads drained. Reports the first divergent offset per buffer.
    if (meshlet_readback_pending_ && stream_frame_ == 50)
    {
        meshlet_readback_pending_ = false;
        vkDeviceWaitIdle(device_.get_device());
        auto check = [&](const char* name, string::gpu::resource_id id, const void* cpu, VkDeviceSize size) {
            const string::gpu::resource_id staging = allocator_.create_resource(string::gpu::buffer_info{
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
                .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                  | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            });
            string::gpu::command_recorder rec;
            rec.init(device_.get_device(), device_.get_queue(string::gpu::queue_type::GRAPHICS));
            VkCommandBuffer cb = rec.begin();
            const VkBufferCopy region = { 0, 0, size };
            vkCmdCopyBuffer(cb, allocator_.get_buffer(id).buffer,
                            allocator_.get_buffer(staging).buffer, 1, &region);
            rec.end().immediate_submit();
            rec.destroy();
            const uint8_t* gpu = static_cast<const uint8_t*>(
                allocator_.get_buffer(staging).allocation_info.pMappedData);
            const uint8_t* ref = static_cast<const uint8_t*>(cpu);
            VkDeviceSize first_bad = size, bad_bytes = 0;
            for (VkDeviceSize i = 0; i < size; ++i)
                if (gpu[i] != ref[i]) { if (first_bad == size) first_bad = i; ++bad_bytes; }
            if (bad_bytes)
                STRING_LOG_WARN("[readback] {} CORRUPT: {} of {} bytes differ, first at {}",
                                name, bad_bytes, size, first_bad);
            else
                STRING_LOG_INFO("[readback] {} clean ({} bytes)", name, size);
            allocator_.destroy_resource(staging);
        };
        check("meshlets", meshlet_buffer_, meshlet_model_.meshlets.data(),
              sizeof(GpuMeshlet) * meshlet_model_.meshlets.size());
        check("meshlet_vertices", meshlet_vertices_, meshlet_model_.meshlet_vertices.data(),
              sizeof(uint32_t) * meshlet_model_.meshlet_vertices.size());
        check("meshlet_triangles", meshlet_triangles_, meshlet_model_.meshlet_triangles.data(),
              sizeof(uint32_t) * meshlet_model_.meshlet_triangles.size());
        // What the GPU actually reads per draw: the MAPPED DrawInfo (post-fill, live).
        for (uint32_t d = 79; d < 87 && draw_info_mapped_; ++d)
        {
            const GpuDrawInfo& mi = draw_info_mapped_[d];
            STRING_LOG_INFO("[mapped] draw {} model[3] ({:.2f},{:.2f},{:.2f}) resident {} lod0 off {} cnt {} lod_count {}",
                d, mi.model[3].x, mi.model[3].y, mi.model[3].z, mi.resident,
                mi.lods[0].meshlet_offset, mi.lods[0].meshlet_count, mi.lod_count);
        }
    }

    // Toggle the debug frozen culling frustum. On freeze, snapshot the current view-projection;
    // the camera keeps moving but the cull test stays against the snapshot, so culled geometry
    // becomes visible as it leaves the frozen view.
    if (input_map_.pressed("freeze_culling"))
    {
        mesh_cull_frozen_ = !mesh_cull_frozen_;
        if (mesh_cull_frozen_)
        {
            mesh_frozen_view_proj_ = camera_.view_proj();
            mesh_frozen_camera_pos_ = camera_.position();  // cone/HiZ/LOD eye freezes too
        }
        STRING_LOG_INFO("Cull frustum {}", mesh_cull_frozen_ ? "FROZEN (debug)" : "live");
    }
    if (input_map_.pressed("toggle_culling"))
    {
        cull_enabled_ = !cull_enabled_;
        STRING_LOG_INFO("GPU frustum culling {}", cull_enabled_ ? "ON" : "OFF (debug)");
    }

    // --- Brief 03 meshlet-path debug controls ---
    if (input_map_.pressed("toggle_hiz"))
    {
        hiz_enabled_ = !hiz_enabled_;
        STRING_LOG_INFO("HiZ occlusion {}", hiz_enabled_ ? "ON" : "OFF");
    }
    if (input_map_.pressed("cycle_debug_view"))
    {
        debug_view_ = (debug_view_ + 1) % 4;
        static const char* names[] = { "none", "meshlet-id", "LOD-level", "occlusion-reject" };
        STRING_LOG_INFO("Meshlet debug view: {}", names[debug_view_]);
    }
    if (input_map_.pressed("toggle_lod"))
    {
        lod_enabled_ = !lod_enabled_;
        STRING_LOG_INFO("Discrete LOD select {}", lod_enabled_ ? "ON" : "OFF (LOD0)");
    }
    if (input_map_.pressed("toggle_crowd"))
    {
        crowd_enabled_ = !crowd_enabled_;
        build_crowd(scene_aabb_min_, scene_aabb_max_);
        STRING_LOG_INFO("Crowd stress scene {}", crowd_enabled_ ? "ON" : "OFF");
    }

    // --- Time-of-day sun + Forward+ debug controls ---
    if (input_map_.pressed("sun_animate"))
    {
        sun_animate_ = !sun_animate_;
        STRING_LOG_INFO("Time-of-day {}", sun_animate_ ? "ANIMATING" : "paused");
    }
    if (sun_animate_) time_of_day_ = std::fmod(time_of_day_ + delta_time * 0.03f, 1.0f);
    if (input_map_.held("time_back")) time_of_day_ = glm::clamp(time_of_day_ - delta_time * 0.15f, 0.0f, 1.0f);
    if (input_map_.held("time_fwd"))  time_of_day_ = glm::clamp(time_of_day_ + delta_time * 0.15f, 0.0f, 1.0f);
    if (input_map_.pressed("toggle_lights"))
    {
        lights_enabled_ = !lights_enabled_;
        STRING_LOG_INFO("Local lights {}", lights_enabled_ ? "ON" : "OFF");
    }
    if (input_map_.pressed("toggle_heatmap"))
    {
        froxel_heatmap_ = !froxel_heatmap_;
        STRING_LOG_INFO("Froxel heatmap {}", froxel_heatmap_ ? "ON" : "OFF");
    }
    // Brief 07: headless TOD pin (r.tod / STRING_TOD) — wins over the scrub keys/animation so
    // captures are deterministic.
    if (const float tod = cv_time_of_day().get(); tod >= 0.0f)
        time_of_day_ = glm::clamp(tod, 0.0f, 1.0f);
    // Brief 07 furnace test lever (r.furnace): shader-side it forces a uniform white environment
    // + white albedo and skips sun/local lights; here it also drives the IBL capture + sky pass.
    furnace_ = cv_furnace().get();
    // Brief 09 fix: the furnace PINS the exposure (beats auto AND manual) so the radiance-1
    // furnace environment renders flat WHITE — under scene exposure it reads as uniform grey,
    // defeating the visual gate. The pin targets exposed = 4.0 (EV ~7.70), NOT 1.0: filmic-style
    // output transforms map scene 1.0 to only ~80-85% display (the shoulder reserves headroom
    // above scene-white; the aces2 CAM DRT sits lower still), so an exposed-1.0 furnace showed as
    // light grey. Two stops above reference white lands the flat field at display white
    // (>= ~0.96 sRGB) through BOTH tonemap curves while staying on the shoulder rather than hard
    // clip, so non-uniformities (the gate's actual signal) remain visible.
    String::CompositePass::set_exposure_override(std::log2(1000.0f / (1.2f * 4.0f)), furnace_);
    // Drive the sun direction + sky palette from the time of day, then refit the cascades to the live
    // camera + sun. Both move, so the cascades are recomputed every frame (stabilization keeps them
    // from shimmering).
    const SunState sun = sun_for_time(time_of_day_);
    sun_dir_ = sun.dir;
    sun_color_ = sun.sun_color;
    sun_intensity_ = sun.sun_intensity;
    sky_zenith_ = sun.sky_zenith;
    sky_ground_ = sun.sky_ground;
    if (draw_count_ > 0)
    {
        compute_cascades();
        animate_lights(delta_time);
    }

    // Brief 07: amortized IBL update trigger. The chain re-runs only when the sun has moved more
    // than ~0.1 deg since the last capture (TOD scrub/animation -> every frame; static sun ->
    // never), when the furnace lever flips, or on the first frame. dbg.ibl_every_frame forces the
    // worst case for cost measurement.
    // IBL update trigger + numeric verification moved to IblPass::update (brief 11 step 3).

    // Brief 09b: probe relight trigger. The relight tracks the sun the same way the sky IBL does
    // (amortized on sun-delta; static sun -> relit once). Gated by r.gi. The static capture runs
    // once in record_compute. The debug-sphere mode is snapshotted here for record().
    probe_debug_mode_ = cv_gi_enabled().get()
        ? static_cast<uint32_t>(std::max(0, cv_gi_probe_debug().get()))
        : 0u;
    if (probe_volume_.valid && cv_gi_enabled().get())
    {
        constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg) — matches the IBL trigger
        const float align = glm::dot(glm::normalize(sun_dir_), probe_relit_sun_dir_);
        // A sun move (or first light) ARMS a full set of converge passes: each amortized full pass
        // propagates the bounce one step further and settles the hysteresis EMA; when the passes
        // run out, relight goes idle until the next trigger (~0 static-sun cost). Relight depends
        // on the sky-SH (miss/fallback source), so the IBL must have primed first.
        if (ibl->primed() && (!probe_primed_ || cv_ibl_every_frame().get() || align < kSunDeltaCos))
        {
            probe_relight_passes_left_ = kRelightConvergePasses;
            probe_relight_pending_ = true;
        }
    }

    // Residency feedback — textures and geometry driven by the SAME per-draw frustum visibility.
    //  - Textures: every streamed texture is pinned to its coarse-tail floor (so the whole scene is
    //    blurry-but-real), and each *visible* draw raises its base-color texture toward the mip its
    //    on-screen coverage needs (screen-coverage LOD). The finest request across all draws sharing
    //    a texture wins; it's aggregated in frame_desired_detail_ and issued once per texture below.
    //    Textures are never released — re-reading from disk on return is cheap but their VRAM fits —
    //    so once resolved they stay.
    //  - Geometry (only when streaming, i.e. the model doesn't fit): want visible draws, release the
    //    rest, so off-screen geometry is evicted and its heap space reused. begin_frame() first
    //    reclaims ranges whose deferred-free window has elapsed.
    for (std::size_t i = 0; i < texture_lod_.size(); ++i)
    {
        frame_desired_detail_[i] = texture_lod_[i].coarse_detail;  // 0 for non-streamed (stb) textures
    }
    const glm::mat4 vp = camera_.view_proj();
    if (geometry_streaming_)
    {
        geometry_streamer_->begin_frame(stream_frame_);
    }
    for (uint32_t d = 0; d < draw_count_; ++d)
    {
        const bool visible = aabb_in_frustum(vp, draws_[d].aabb_min, draws_[d].aabb_max);
        if (visible && draws_[d].material >= 0)
        {
            const int tex = materials_[draws_[d].material].base_color_texture;
            if (tex >= 0 && texture_lod_[tex].levels > 0)
            {
                const std::uint32_t want = desired_detail(vp, draws_[d].aabb_min, draws_[d].aabb_max,
                                                          screen_size, texture_lod_[tex].levels,
                                                          texture_lod_[tex].base_extent,
                                                          texture_lod_[tex].coarse_detail);
                frame_desired_detail_[tex] = std::max(frame_desired_detail_[tex], want);
            }
        }
        if (geometry_streaming_)
        {
            if (visible)
                geometry_residency_->want(d, 1, string::gpu::resource_priority::LAZY, stream_frame_);
            else
                geometry_residency_->release(d);
        }
    }
    // Issue one want() per streamed texture with its aggregated desired detail. A texture wanted
    // above its coarse floor this frame is on screen and needs sharpening now → IMMEDIATE; one still
    // at the floor is background → LAZY (so it fills in without stalling the visible ones).
    for (std::size_t i = 0; i < texture_lod_.size(); ++i)
    {
        if (texture_lod_[i].levels == 0)
        {
            continue;  // non-streamed (stb) texture
        }
        const bool on_screen = frame_desired_detail_[i] > texture_lod_[i].coarse_detail;
        const auto priority = on_screen ? string::gpu::resource_priority::IMMEDIATE
                                        : string::gpu::resource_priority::LAZY;
        residency_.want(texture_images_[i], frame_desired_detail_[i], priority, stream_frame_);
    }
    residency_.tick(stream_frame_);
    if (draw_count_ > 0 && geometry_streaming_)
    {
        geometry_residency_->tick(stream_frame_);
        if (stream_frame_ == 1 || stream_frame_ == 5 || stream_frame_ == 60 || stream_frame_ == 300)
        {
            STRING_LOG_INFO("[geo] frame {}: {} of {} draws resident, {} MB streamed, {} evictions",
                            stream_frame_, geometry_streamer_->resident_count(), draw_count_,
                            geometry_streamer_->streamed_bytes() / (1024 * 1024),
                            geometry_streamer_->evicted_count());
        }
    }

    // One-shot: report the frame at which every streamed texture reached full residency (all CACHED
    // at their desired detail), so streaming progress is observable against the load logs.
    if (!logged_full_resident_ && !streamed_textures_.empty())
    {
        bool all_full = true;
        for (const string::gpu::resource_id id : streamed_textures_)
        {
            if (residency_.status_of(id) != string::gpu::stream_status::CACHED)
            {
                all_full = false;
                break;
            }
        }
        if (all_full)
        {
            STRING_LOG_INFO("[stream] all {} textures fully resident at frame {}",
                            streamed_textures_.size(), stream_frame_);
            logged_full_resident_ = true;
        }
    }

    // --- Forward+ per-frame GPU data (this frame's ring slot) ----------------------------------
    if (draw_count_ > 0 && scene_buffer_.valid() && current_frame < frames_in_flight_)
    {
        // froxel index-buffer capacity is ensured in FroxelPass::update now (brief 11 step 3).

        // Upload this frame's animated lights (or none, if the stress set is toggled off).
        const uint32_t light_count = lights_enabled_ ? static_cast<uint32_t>(lights_.size()) : 0u;
        if (light_count > 0)
        {
            std::memcpy(resources->mapped(light_buffer_, current_frame), lights_.data(),
                        sizeof(GpuLight) * light_count);
        }

        // Fill SceneData for this frame. The lit shader reads sun/ambient/CSM/froxel state from here.
        SceneData scene{};
        scene.camera_pos = camera_.position();
        scene.exposure = String::CompositePass::exposure_scale();   // for display-referred debug views
        scene.sun_dir = sun_dir_;
        scene.sun_intensity = sun_intensity_;
        scene.sun_color = sun_color_;
        scene.ambient_sky = sky_zenith_;
        // sky_ground_ is an ALBEDO now; mirror sky_ground_radiance() (sky.slang) so this field
        // keeps its "hemispheric ambient, down" radiance meaning (unused by the lit shader since
        // the brief-07 IBL, but kept coherent).
        {
            const float lum = glm::dot(sky_zenith_, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            const glm::vec3 horizon = glm::mix(sky_zenith_, glm::vec3(lum), 0.6f) * 2.0f;
            const glm::vec3 e_sun = sun_color_ * sun_intensity_
                                    * glm::clamp(glm::normalize(sun_dir_).y, 0.0f, 1.0f);
            const glm::vec3 e_sky = glm::pi<float>() * 0.5f * (sky_zenith_ + horizon);
            scene.ambient_ground = sky_ground_ / glm::pi<float>() * (e_sun + e_sky);
        }
        for (uint32_t c = 0; c < settings_.cascade_count; ++c)
        {
            scene.cascade_view_proj[c] = cascade_view_proj_[c];
            scene.cascade_split[c] = glm::vec4(cascade_split_[c], 0, 0, 0);
            scene.cascade_texel[c] = glm::vec4(cascade_world_texel_[c], 0, 0, 0);
            scene.cascade_slot[c] = glm::uvec4(shadow_slots_[current_frame][c], 0, 0, 0);
        }
        // Brief 11 M3 graceful degrade: when the shadow.cascades pass is toggled off (r.pass.shadow),
        // report cascade_count 0 so the lit shader's shadow term cancels (select_cascade -> kNoCascade
        // -> unshadowed) instead of sampling stale/undefined maps. Data-level degrade — the bindless
        // shadow reads go through this SceneData field, not a graph-bound descriptor.
        scene.cascade_count = cv_pass_shadow().get() ? settings_.cascade_count : 0u;
        scene.shadow_texel = 1.0f / static_cast<float>(settings_.shadow_resolution);
        scene.shadow_bias = cv_shadow_bias().get();
        scene.shadow_normal_offset_scale = cv_shadow_normal_offset().get();
        scene.cascade_blend = settings_.cascade_blend;
        scene.view = camera_.view();
        // froxel grid dims + buffer address come from FroxelPass's component, published in the scene.
        const uint32_t froxel_tx = froxel ? froxel->tiles_x() : 0;
        const uint32_t froxel_ty = froxel ? froxel->tiles_y() : 0;
        scene.froxel_dims = glm::uvec4(froxel_tx, froxel_ty, kFroxelDepthSlices, kFroxelTileSize);
        const float near_p = camera_.near_plane();
        const float far_p = std::min(settings_.shadow_depth_range, camera_.far_plane());
        scene.froxel_planes = glm::vec4(near_p, far_p, 1.0f / std::log(far_p / near_p),
                                        static_cast<float>(light_count));
        scene.lights = light_count > 0 ? resources->address(light_buffer_, current_frame) : 0;
        scene.froxels = froxel ? froxel->froxels_address(current_frame) : 0;
        scene.max_lights_per_froxel = kMaxLightsPerFroxel;
        scene.debug_flags = (froxel_heatmap_ ? 1u : 0u) | (furnace_ ? 2u : 0u)
                          | (cv_gtao_spec_occ().get() ? 0u : 4u)    // bit2: disable bent-normal spec-occ
                          | ((static_cast<uint32_t>(std::max(cv_light_debug().get(), 0)) & 0xFu) << 4);  // bits4-7: lighting isolate
        // Brief 07: the sky-IBL products (single-buffered; the update chain is ordered against
        // in-flight readers inside record_ibl_update / by the declared SH usages).
        scene.sh = ibl->sh_address();
        scene.env_slot = ibl->env_slot();
        scene.env_mips = ibl->env_mips();
        scene.dfg_slot = ibl->dfg_slot();

        // Brief 09 GTAO: decide HERE (pre-record) whether the chain runs this frame — the shader
        // reads gtao_slot from this SceneData, so the decision and the recording must agree.
        // Needs the PREVIOUS slot's resolved depth (two-phase HiZ path); descriptor updates for
        // (re)created targets land in ensure_gtao(), safely before any set bind this frame.
        // The furnace test must be a flat white background at every roughness/metallic — GTAO on
        // the furnace must read as vis == 1 (the brief-09 gate). Rather than trust the AO pass to
        // produce exactly 1.0 over a depthful furnace scene, disable the whole chain under furnace
        // so ambient occlusion cannot perturb the uniform-white acceptance state.
        const bool gtao_allowed = cv_gtao_enabled().get() && !furnace_;
        if (meshlet_program_ && draw_info_mapped_ && gtao_program_ != nullptr && gtao_allowed)
            ensure_gtao();
        // Brief 11 step 3: ensure the HiZ pyramid HERE (in update()) too — its (re)build binds new
        // per-mip storage views into the bindless set, and now that IblPass records its compute BEFORE
        // this pass, that descriptor update must land in the update phase (before any set bind this
        // frame), not in record_compute. ensure_hiz is idempotent, so the record_compute calls no-op.
        if (meshlet_program_ && draw_info_mapped_)
            ensure_hiz(current_frame);
        const uint16_t gtao_prev = static_cast<uint16_t>(
            (current_frame + frames_in_flight_ - 1) % frames_in_flight_);
        gtao_runs_this_frame_ = gtao_allowed && gtao_program_ != nullptr
            && gtao_denoise_program_ != nullptr && gtao_raw_ != 0
            && gtao_prev < hz_depth_valid_.size() && hz_depth_valid_[gtao_prev] != 0;
        scene.prev_view_proj = gtao_runs_this_frame_ ? gtao_slot_view_proj_[gtao_prev]
                                                     : glm::mat4(1.0f);
        scene.gtao_slot = gtao_runs_this_frame_ ? gtao_final_sampled_slots_[current_frame]
                                                : 0xFFFFFFFFu;
        scene.gtao_strength = std::clamp(cv_gtao_strength().get(), 0.0f, 1.0f);
        scene.gtao_w = gtao_size_.x;   // half-res AO extent for the lit shader's joint upsample
        scene.gtao_h = gtao_size_.y;

        // Brief 09b probe GI. probe_gi gates the whole shading-side feature: 0 under r.gi 0 (the
        // pixel-parity lever — the shader then takes the pre-09b sky-SH path bit-identically),
        // under r.furnace (the furnace validates the BRDF against the analytic environment, same
        // rule as GTAO), and until the capture + FIRST FULL relight pass complete (the atlas is
        // zero-cleared before that — sampling it would darken instead of falling back).
        const bool probe_gi_on = probe_volume_.valid && cv_gi_enabled().get() && !furnace_
                              && probe_captured_ && probe_primed_;
        scene.probe_origin = probe_volume_.origin;
        scene.probe_spacing = probe_volume_.spacing;
        scene.probe_counts = probe_volume_.counts;
        scene.probe_irrad_slot = probe_irrad_sample_slot_;
        scene.probe_vis_slot = probe_vis_sample_slot_;
        // probe_gi: 0 off, 1 normal probe-GI shading, 2 = GI-debug isolate view (indirect diffuse
        // x albedo only) — the reference-free-scene judging tool (r.gi.debug 1).
        scene.probe_gi = probe_gi_on ? (cv_gi_debug().get() != 0 ? 2u : 1u) : 0u;
        scene.probe_offsets = probe_offset_ != 0 ? allocator_.get_buffer(probe_offset_).device_address : 0;
        scene.probe_active = probe_active_ != 0 ? allocator_.get_buffer(probe_active_).device_address : 0;
        scene.probe_occluded_floor = std::clamp(cv_gi_occluded_floor().get(), 0.0f, 1.0f);

        std::memcpy(resources->mapped(scene_buffer_, current_frame), &scene, sizeof(SceneData));
    }

    // Crowd built lazily the first time it's enabled once geometry residency is known (so crowd
    // copies inherit the base draws' resident flags). Cheap no-op once active_draw_count_ reflects it.
    if (draw_info_mapped_ && crowd_enabled_ && active_draw_count_ == base_draw_count_
        && base_draw_count_ > 0)
    {
        build_crowd(scene_aabb_min_, scene_aabb_max_);
    }

    // --- Brief 03b: LOD select moved to the GPU draw-cull compute (record_draw_cull). Here we only
    // read back the stats the GPU wrote earlier + publish the overlay state. ---
    if (draw_info_mapped_)
    {
        // Read back the stats the GPU wrote two frames ago (this slot's buffer was filled last time
        // this frame index ran, i.e. frames_in_flight frames back — safe, no stall).
        if (stats_buffer_.valid() && current_frame < frames_in_flight_)
        {
            const void* mapped = resources->mapped(stats_buffer_, current_frame);
            if (mapped) std::memcpy(&stats_latest_, mapped, sizeof(GpuMeshStats));
        }

        // Publish the overlay state for the UI author (shared_ptr seam; same render thread).
        if (overlay_stats_)
        {
            overlay_stats_->stats = stats_latest_;
            overlay_stats_->hiz_enabled = hiz_enabled_;
            overlay_stats_->crowd_enabled = crowd_enabled_;
            overlay_stats_->debug_view = debug_view_;
            overlay_stats_->total_meshlets = meshlet_model_.total_meshlets;
            overlay_stats_->draw_count = draw_count_;

            // Brief 06: publish the camera + scene snapshot for the debug-line pass and the
            // scene/draw inspector. view_proj/camera + lights refresh every frame; the draw table is
            // rebuilt only when its size changes (load / crowd toggle) — the per-draw geometry
            // (bounds, meshlet/LOD counts) is static, only the resident flag is live.
            overlay_stats_->view_proj = camera_.view_proj();
            overlay_stats_->camera_pos = camera_.position();

            if (overlay_stats_->draws.size() != meshlet_model_.draws.size())
            {
                overlay_stats_->draws.clear();
                overlay_stats_->draws.reserve(meshlet_model_.draws.size());
                for (uint32_t d = 0; d < meshlet_model_.draws.size(); ++d)
                {
                    const GpuDrawInfo& info = meshlet_model_.draws[d];
                    InspectorDraw id;
                    id.name = "draw " + std::to_string(d);
                    id.index = d;
                    id.material = -1;
                    id.meshlet_count = info.total_meshlets;
                    id.lod_count = info.lod_count;
                    id.aabb_min = info.center - glm::vec3(info.radius);
                    id.aabb_max = info.center + glm::vec3(info.radius);
                    overlay_stats_->draws.push_back(std::move(id));
                }
            }
            // Live residency flag per draw (streamer writes it into draw_info_mapped_).
            if (draw_info_mapped_)
                for (uint32_t d = 0; d < overlay_stats_->draws.size(); ++d)
                    overlay_stats_->draws[d].resident = draw_info_mapped_[d].resident != 0;

            overlay_stats_->lights.clear();
            overlay_stats_->lights.reserve(lights_.size());
            for (const GpuLight& L : lights_)
            {
                InspectorLight il;
                il.position = glm::vec3(L.position_radius);
                il.range = L.position_radius.w;
                il.color = glm::vec3(L.color_intensity);
                il.spot = L.direction_type.w > 0.5f;
                overlay_stats_->lights.push_back(il);
            }
        }

        // Periodic culling-stats log (the numbers the UI overlay also shows). Sampled a few times so
        // the smoke run captures meaningful frustum/cone/HiZ reductions without spamming.
        if (stream_frame_ == 30 || stream_frame_ == 120 || stream_frame_ == 600)
        {
            const GpuMeshStats& s = stats_latest_;
            // Brief 04d: with two-phase active, meshlets_total/after_frustum/after_cone DOUBLE-count
            // (the task shader runs once per phase per meshlet). after_hiz = phase1+phase2 DRAWN
            // (the union == the visible set); phase2 = the disocclusion complement drawn this frame.
            STRING_LOG_INFO("[mesh-cull] frame {}: meshlets {} -> frustum {} -> cone {} -> hiz {} "
                            "(phase2 {}) (LOD draws {}/{}/{}/{}){}",
                            stream_frame_, s.meshlets_total, s.after_frustum, s.after_cone, s.after_hiz,
                            s.phase2_drawn,
                            s.draws_per_lod[0], s.draws_per_lod[1], s.draws_per_lod[2], s.draws_per_lod[3],
                            crowd_enabled_ ? " [crowd]" : "");
            // Brief 04 M4: per-cascade shadow draw-cull. "before" = every resident draw dispatched for
            // every cascade (resident_draws x cascade_count); "after" = draws surviving the per-cascade
            // light-frustum reject (stats[9], summed across cascades).
            const uint32_t shadow_before = active_draw_count_ * settings_.cascade_count;
            STRING_LOG_INFO("[shadow-cull] cascades {}: shadow draws {} -> {} (per-cascade light-sphere reject)",
                            settings_.cascade_count, shadow_before, s.shadow_draws);
            // Brief 07 amortization honesty: how many frames actually re-ran the IBL chain.
            STRING_LOG_INFO("[ibl] frame {}: {} env updates so far ({} static sun -> 1 expected)",
                            stream_frame_, ibl->update_count(), sun_animate_ ? "animating" : "");
        }
    }

    ++stream_frame_;

    // --- Brief 04e M2: declare this frame's inter-phase buffer usages -------------------------
    // The frame-level graph derives the compute->draw barriers (worklists, froxel lists) and the
    // cross-frame visibility-bitfield ordering from THESE declarations; hand-rolled equivalents
    // are deleted. Shadow worklists + scan scratch are intra-pass (written AND consumed inside
    // record_compute, local barriers) so they are not frame-graph state.
    usages.resize(static_usage_count_);
    const auto declare_worklist = [this](const Worklist& wl) {
        if (wl.buffer == 0) return;
        usages.push_back({ wl.buffer, String::Access::StorageWrite,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
        usages.push_back({ wl.buffer, String::Access::IndirectRead,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT });
        usages.push_back({ wl.buffer, String::Access::StorageRead,
                           VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT });
    };
    if (current_frame < wl_opaque_.size()) declare_worklist(wl_opaque_[current_frame]);
    if (current_frame < wl_twosided_.size()) declare_worklist(wl_twosided_[current_frame]);
    // The froxel list's async_usages (write on the async chain, read by the main-queue fragment stage)
    // are declared in FroxelPass::update now (brief 11 step 3).
    // Per-frame stats: reset + histogram in compute, then atomic tallies from the task/mesh/
    // fragment stages of the draws. Declaring the draw-stage RMW gives it a derived
    // compute->draw-stages WAW barrier (the old broad hand barrier never covered the TASK/MESH
    // stats atomics — a latent narrowness this closes).
    if (stats_buffer_.valid() && current_frame < frames_in_flight_)
    {
        // Brief 16: declare the LOGICAL stats handle; the executor resolves the per-frame physical.
        usages.push_back({ .access = String::Access::StorageWrite,
                           .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .buf = stats_buffer_ });
        usages.push_back({ .access = String::Access::StorageWrite,
                           .stage = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT
                               | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT
                               | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .buf = stats_buffer_ });
    }
    // Brief 07: the SH coefficient buffer — written by the IBL compute chain on update frames,
    // read by every lit fragment. The graph derives the compute->fragment barrier (and the
    // cross-frame ordering) from these declarations; the cubemap/DFG images use intra-pass
    // barriers (documented local class, like the HiZ mip chain).
    // CONSUMER side of the SH buffer: every lit fragment reads it. The producer write is declared by
    // IblPass; the graph derives the compute->fragment barrier from the two declarations (brief 11 step 3).
    if (ibl && ibl->sh_buffer() != 0)
        usages.push_back({ ibl->sh_buffer(), String::Access::StorageRead,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT });
    // The persistent visibility bitfield: task-stage read-modify-write every two-phase frame.
    // StorageWrite's scope is READ|WRITE, so the derived TASK->TASK barrier orders the previous
    // frame's phase-2 writes before BOTH this frame's phase-1 reads (RAW) and phase-2 writes
    // (WAW) — the 04d cross-frame flicker fix, now declaration-driven.
    if (hiz_enabled_ && hiz_program_ && visbits_buffer_ != 0)
        usages.push_back({ visbits_buffer_, String::Access::StorageWrite,
                           VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT });
}

bool GeometryPass::record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (draw_count_ == 0)
    {
        return false;
    }

    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    // HiZ (re)build's descriptor updates (per-mip storage views into the no-UPDATE_AFTER_BIND set)
    // now land in update() (brief 11 step 3) — before IblPass binds the set. This idempotent call
    // no-ops unless the screen changed since update() ran this frame.
    if (meshlet_program_ && draw_info_mapped_) ensure_hiz(current_frame);

    // GTAO now runs in the standalone GtaoPass (prepass, before this pass) via record_gtao_if_enabled
    // — brief 11 step 3. ensure_gtao()'s descriptor updates still land in update() (before any bind).

    // The sky-IBL update chain runs in the standalone IblPass (prepass compute, ordered before this
    // pass so probe relight below reads this frame's SH) — brief 11 step 3.

    // Brief 11 M2: the probe-GI capture + relight moved OUT to record_gi_if_enabled, scheduled by the
    // standalone GiPass (compute prepass, ordered after IblPass so relight's SH source is this frame's,
    // and before ShadowPass so relight's shadow-map reads precede this frame's shadow depth writes —
    // the WAR execution edge in record_probe_relight relies on that order, preserved by authoring).

    // (Brief 04e M2: the cross-frame visibility-bitfield barrier that lived here is now DERIVED
    // by the renderer from this pass's declared {visbits, StorageWrite, TASK} usage — see the
    // usage declarations at the end of update(). Intra-frame phase-1 -> phase-2 ordering is still
    // handled locally in record_between.)

    // --- Brief 03: meshlet-path per-frame prep (stats reset, HiZ pyramid) --------------------------
    // Builds the GPU work lists, the HiZ pyramid, and the shadow cascades that record() then draws.
    if (meshlet_program_ && draw_info_mapped_)
    {
        ensure_hiz(current_frame);

        // Zero this frame's stats.
        if (reset_program_)
        {
            const string::gpu::pipeline& rp = reset_program_->current();
            const ResetPush rpush{
                .stats = resources->address(stats_buffer_, current_frame),
                .stats_words = sizeof(GpuMeshStats) / 4,
                ._pad = 0,
            };
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, rp.pipeline);
            vkCmdPushConstants(command_buffer, rp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ResetPush), &rpush);
            vkCmdDispatch(command_buffer, 1, 1, 1);
            // Barrier: the stats reset must land before the draw-cull compute's histogram atomics.
            const VkMemoryBarrier2 sb = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            };
            const VkDependencyInfo sdep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1, .pMemoryBarriers = &sb };
            vkCmdPipelineBarrier2(command_buffer, &sdep);
        }

        // Brief 04c: build the meshlet worklists for this frame in two phases. DRAW phase writes the
        // per-draw counts + shared LOD; EXPANSION phase scans them into compacted worklists + indirect
        // groupCounts. Camera worklists (opaque + two-sided) feed the main pass; prepass worklists
        // (forced LOD0) feed the HiZ depth prepass; per-cascade shadow worklists feed the cascades.
        if (draw_cull_program_ && expand_fill_program_ && active_draw_count_ > 0)
        {
            // Brief 04d: the depth-only HiZ prepass is DELETED. Phase 1 renders last-frame-visible
            // meshlets directly into the MSAA targets; the pyramid is MIN-resolved from that depth in
            // record_between(). So there are no prepass worklists — only the camera opaque/two-sided
            // lists (shared by phase 1 and phase 2 — the task shader partitions them per phase) plus
            // the per-cascade shadow lists.
            const bool do_shadow = !shadow_images_.empty();
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "draw-cull")
                record_draw_cull(recorder, current_frame);
                if (do_shadow)
                    for (uint32_t c = 0; c < settings_.cascade_count && c < kMaxCascades; ++c)
                        record_draw_cull(recorder, current_frame, /*cascade*/ int(c));
            }
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "expand")
                const VkDeviceAddress cam_lod =
                    draw_lod_address(current_frame);
                record_expand(recorder, wl_opaque_[current_frame], cam_lod);
                record_expand(recorder, wl_twosided_[current_frame], cam_lod);
                if (do_shadow)
                    for (uint32_t c = 0; c < settings_.cascade_count && c < kMaxCascades; ++c)
                        record_expand(recorder, wl_shadow_[current_frame][c], cam_lod);
            }
        }

        // Brief 04d: the depth-only HiZ prepass + pyramid build are GONE from here. Phase 1 (record())
        // renders last-frame-visible meshlets directly into the MSAA targets; the renderer then MIN-
        // resolves that MSAA depth into hz.depth and calls record_between() to build the pyramid, then
        // record_after_between() draws phase 2. This method only decides whether the two-phase path is
        // active this frame and clears the persistent bitfield when required.
        //
        // two_phase_active_ gates breaks_scene_group(): active only when HiZ is enabled AND geometry is
        // resident (warmup done). When inactive (HiZ off, or the first frames) the pass renders in ONE
        // group, single-pass (phase 0) — everything unconditionally, exactly like HiZ-off before.
        two_phase_active_ = hiz_enabled_ && hiz_program_ && stream_frame_ > 1;

        // Brief 11 P2 (B1): name this frame's MSAA-depth resolve target as a declared usage (replacing
        // the depth_resolve_target() hook). Appended here — AFTER update() finalized `usages` and the
        // graph was built — because the two-phase decision isn't known until now; the renderer's group
        // loop (which runs after this record_compute) scans for it. Dropped + re-added each frame
        // (update() truncates back to static_usage_count_). Marker usage: not a graph write, untracked.
        if (two_phase_active_ && hiz_depth_ring_.valid())
            usages.push_back({ .access = String::Access::DepthResolve,
                               .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               .img = hiz_depth_ring_ });

        // Clear the persistent visibility bitfield + last-LOD on demand (first frame, teleport, a full
        // streaming reset). A cleared bitfield is CORRECT — every meshlet takes phase 2 for one frame.
        // Freeze (F) must NOT clear (it would erase the frozen visibility the debug eye is inspecting).
        if (visbits_clear_pending_ && visbits_buffer_ && !mesh_cull_frozen_)
        {
            const string::gpu::allocated_buffer& vb = allocator_.get_buffer(visbits_buffer_);
            vkCmdFillBuffer(command_buffer, vb.buffer, 0, VK_WHOLE_SIZE, 0u);
            const string::gpu::allocated_buffer& pl = allocator_.get_buffer(prev_draw_lod_buffer_);
            vkCmdFillBuffer(command_buffer, pl.buffer, 0, VK_WHOLE_SIZE, 0u);
            const VkMemoryBarrier2 fb = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            };
            const VkDependencyInfo fdep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1, .pMemoryBarriers = &fb };
            vkCmdPipelineBarrier2(command_buffer, &fdep);
            visbits_clear_pending_ = false;
        }
    }

    // (Brief 04e M4: the froxel light-binning dispatch moved to record_async_compute — it is
    // dependency-free within the frame, so the renderer places it on an async compute lane when
    // one exists, or records it inline on the main queue otherwise.)
    // Brief 11 M2: the cascaded shadow-map depth render moved OUT to record_shadows_if_enabled,
    // scheduled by the standalone ShadowPass (compute prepass, ordered after this pass so its
    // record_compute runs after the draw-cull/expand above produced the per-cascade worklists).
    return true;
}

// Brief 11 M2: probe-GI static capture (once) + dynamic relight (amortized). Extracted from
// record_compute's head into its own public method, scheduled by the standalone GiPass in the compute
// prepass — ordered after IblPass (relight's sky-SH source is this frame's) and before ShadowPass (the
// relight->shadow WAR edge). Byte-identical to its old record_compute home; all barriers are internal.
// Returns true if it recorded work.
bool GeometryPass::record_gi_if_enabled(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (!probe_volume_.valid || !cv_gi_enabled().get() || probe_clear_program_ == nullptr) return false;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    bool did_work = false;
    if (!probe_captured_)
    {
        STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "gi-capture")
        record_probe_capture(recorder);
        did_work = true;
    }
    // Relight starts only after the capture COMPLETES — partially-captured probes would relight from
    // the cleared (all-sky-miss) capture and read as outdoor probes until their capture landed. It also
    // guarantees the CSM shadow maps this slot samples have been rendered at least once. record_probe_
    // relight manages the cursor/pending state itself (pending stays true until the armed passes drain).
    if (probe_captured_ && probe_relight_pending_ && ibl->primed())
    {
        STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "gi-relight")
        record_probe_relight(recorder, current_frame);
        did_work = true;
    }
    return did_work;
}

// Brief 11 M2: cascaded shadow maps — render scene depth from the sun into each cascade's image, then
// transition each to SHADER_READ for the lit pass. Draws the resident set via the meshlet shadow path
// (off-screen casters included — cascades read the resident-only work list, NOT camera-culled). Called
// by ShadowPass in the compute prepass; the block is byte-identical to its old record_compute home.
bool GeometryPass::record_shadows_if_enabled(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (shadow_images_.empty() || !meshlet_shadow_program_ || !draw_info_mapped_) return false;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "shadow-cascades")
    VkDescriptorSet set = descriptor_table_.get_set();
    const uint32_t res = settings_.shadow_resolution;
    for (uint32_t c = 0; c < settings_.cascade_count; ++c)
    {
        const string::gpu::allocated_image& shadow = allocator_.get_image(shadow_images_[current_frame][c]);
        vku::transition_image(command_buffer, {
            .image = shadow.image,
            .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        });

        const VkRenderingAttachmentInfo depth_att = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = shadow.view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .depthStencil = { 0.0f, 0 } },  // reverse-Z far plane = 0
        };
        const VkRenderingInfo shadow_render = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { { 0, 0 }, { res, res } },
            .layerCount = 1,
            .colorAttachmentCount = 0,
            .pColorAttachments = nullptr,
            .pDepthAttachment = &depth_att,
        };
        vkCmdBeginRendering(command_buffer, &shadow_render);

        const VkViewport vp = { 0.0f, 0.0f, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f };
        const VkRect2D sc = { { 0, 0 }, { res, res } };
        vkCmdSetViewport(command_buffer, 0, 1, &vp);
        vkCmdSetScissor(command_buffer, 0, 1, &sc);

        // Meshlet shadow path: task/mesh depth-only, frustum-culled per cascade (no HiZ, no
        // cone cull — backfacing meshlets still cast). Brief 03b: reuses the CAMERA work list
        // (same {draw_index, camera-selected LOD} records the main pass uses); the per-cascade
        // meshlet frustum cull happens in the shadow task shader against this light_view_proj.
        const string::gpu::pipeline& msh = meshlet_shadow_program_->current();
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, msh.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            msh.pipeline_layout, 0, 1, &set, 0, nullptr);
        MeshletShadowPush mspush{};
        mspush.light_view_proj = cascade_view_proj_[c];
        mspush.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
        mspush.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
        mspush.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
        mspush.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
        mspush.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
        // Brief 04c: each cascade draws its OWN worklist (resident-only, draw-culled vs THIS
        // cascade's light sphere; camera-selected LOD via the shared draw_lod). One indirect draw.
        const Worklist& sh_wl = wl_shadow_[current_frame][c];
        const VkDeviceAddress sh_base = allocator_.get_buffer(sh_wl.buffer).device_address + sh_wl.offset;
        mspush.records = sh_base + wl_records_off_;   // compacted {draw_index, camera-selected LOD}
        const VkBuffer sh_buf = allocator_.get_buffer(sh_wl.buffer).buffer;
        vkCmdPushConstants(command_buffer, msh.pipeline_layout, VK_SHADER_STAGE_ALL,
                           0, sizeof(MeshletShadowPush), &mspush);
        vkCmdDrawMeshTasksIndirectCountEXT(command_buffer, sh_buf, sh_wl.offset + wl_commands_off_, sh_buf, sh_wl.offset + wl_count_off_,
                                           cull_max_draws_, sizeof(uint32_t) * 3);

        vkCmdEndRendering(command_buffer);

        vku::transition_image(command_buffer, {
            .image = shadow.image,
            .old_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        });
    }
    return true;
}

void GeometryPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    if (draw_count_ == 0)
    {
        return;
    }

    // The procedural sky was drawn by the standalone SkyPass (first pass of this MSAA group) —
    // brief 11 step 3. GeometryPass::record() now starts straight at the meshlet draw path.

    // --- Brief 03/04d: task/mesh meshlet draw path (the scene geometry front-end) ----------------
    if (meshlet_program_ && draw_info_mapped_)
    {
        if (two_phase_active_)
        {
            // Brief 04d PHASE 1: render meshlets marked visible LAST frame (bit set) directly into the
            // MSAA color+depth. No HiZ test (the pyramid isn't built yet). The renderer then MIN-resolves
            // this depth -> hz.depth, record_between() builds the pyramid, record_after_between() draws
            // phase 2 (the disocclusion complement) + transparency.
            STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
            record_opaque_phase(recorder, current_frame, /*phase*/ 1u);
        }
        else
        {
            // Single-pass (HiZ off / warmup): frustum+cone+HiZ in one group, then transparency — the
            // legacy phase-0 flow. breaks_scene_group() is false so this all lands in one render group.
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
                record_opaque_phase(recorder, current_frame, /*phase*/ 0u);
            }
            // Brief 09b: probe-debug spheres after opaque. Brief 11 M2: transparency moved OUT to the
            // standalone TransparencyPass (drawn last in the reopened MSAA group — same position).
            record_probe_debug(recorder);
        }
    }
}

// Brief 04d: draw the opaque one-sided + two-sided camera lists at `phase` (0 legacy / 1 bit-set /
// 2 bit-clear+HiZ+update). Assumes an MSAA scene render pass is already open (record() for phase 1/0,
// record_after_between() for phase 2). Shared by both phases — the task shader does the partitioning.

// Brief 04d: the depth-resolve target the renderer MIN-resolves the phase-1 MSAA depth into (the
// pyramid's single-sample mip-0 source). Valid only when the two-phase path is active this frame.
// Brief 04d: build the HiZ pyramid from the MIN-resolved phase-1 depth (hz.depth), OUTSIDE rendering.
// The renderer already resolved msaa_depth_ -> hz.depth (reverse-Z MIN = farthest) at phase-1's
// EndRendering and left it in DEPTH_ATTACHMENT layout. Reuses the exact mip-0 uniform-stretch +
// 2x2 min chain the old prepass path used; only the depth SOURCE changed (resolve, not a re-draw).
void GeometryPass::record_hiz(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (!two_phase_active_ || !hiz_program_ || current_frame >= hiz_.size()) return;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    const HizPyramid& hz = hiz_[current_frame];
    // Brief 11 step 2b: the inter-pass barriers this method used to hand-roll are GRAPH-DERIVED now:
    //   - hz.depth DEPTH_ATTACHMENT->SHADER_READ, waiting on the EndRendering MIN-resolve: from
    //     hiz.build's SampledRead(hz.depth) usage + the renderer's post-resolve tracker seed;
    //   - the pyramid UNDEFINED->GENERAL: from hiz.build's StorageImageWrite(pyramid) usage;
    //   - the pyramid GENERAL->SHADER_READ for phase-2's task shader: from geometry.phase2's
    //     SampledRead(pyramid) usage;
    //   - the phase1-read -> phase2-RMW visibility-bitfield serialization: from phase1/phase2's
    //     visbits usages (tracker WAW at the task stage).
    // Only the INTRA-pass per-mip compute->compute barriers below remain — this pass building its own
    // multi-mip resource, a per-mip split the single-state tracker deliberately does not model.
    const string::gpu::pipeline& hp = hiz_program_->current();
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline);
    VkDescriptorSet set = descriptor_table_.get_set();
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline_layout,
                            0, 1, &set, 0, nullptr);
    for (uint32_t m = 0; m < hz.mips; ++m)
    {
        const uint32_t dw = std::max(1u, hz.size.x >> m);
        const uint32_t dh = std::max(1u, hz.size.y >> m);
        HizPush hpush{};
        if (m == 0)
        {
            hpush.src_slot = hz.depth_slot;
            hpush.src_size = glm::uvec2(screen_size.width, screen_size.height);
            hpush.copy_depth = 1;
            hpush.src_level = 0;   // the scene-depth image has a single level
        }
        else
        {
            hpush.src_slot = hz.sample_slot;
            hpush.src_size = glm::uvec2(std::max(1u, hz.size.x >> (m - 1)), std::max(1u, hz.size.y >> (m - 1)));
            hpush.copy_depth = 0;
            hpush.src_level = m - 1;   // reduce the previous pyramid level (the whole-chain view exposes all mips)
        }
        hpush.dst_slot = hz.mip_storage_slots[m];
        hpush.dst_size = glm::uvec2(dw, dh);
        vkCmdPushConstants(command_buffer, hp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(HizPush), &hpush);
        vkCmdDispatch(command_buffer, (dw + 7) / 8, (dh + 7) / 8, 1);
        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        vkCmdPipelineBarrier2(command_buffer, &dep);
    }
    // (The pyramid GENERAL->SHADER_READ handoff to phase 2's task shader is GRAPH-DERIVED now — see the
    // note at the top of this method.)

    // Return hz.depth to DEPTH_ATTACHMENT — its CROSS-FRAME resting layout. This is NOT intra-frame
    // inter-pass sync (that edge, the resolve->hiz read, is graph-derived): hz.depth is consumed one
    // frame LATE by GTAO reprojection (a DIFFERENT frame-in-flight slot, record_gtao) and re-written by
    // next cycle's MIN-resolve, both of which hand-manage it from the DEPTH_ATTACHMENT resting layout.
    // That 1-frame-late cross-slot lifecycle is outside the intra-frame graph's model, so the resting
    // layout is restored here by hand (leaving it in SHADER_READ desyncs GTAO's old_layout assumption ->
    // VUID-09600). The tracker's seed re-establishes DEPTH_ATTACHMENT each cycle at the resolve.
    {
        const string::gpu::allocated_image& depth = allocator_.get_image(hz.depth);
        vku::transition_image(command_buffer, {
            .image = depth.image, .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        });
    }

    // Brief 09: this slot's hz.depth now holds this frame's resolved phase-1 depth — capture the
    // matrices GTAO will reproject through when it consumes this slot NEXT frame. Rendering uses
    // the LIVE camera even under freeze-cull, so these are the true depth-buffer transforms.
    if (current_frame < hz_depth_valid_.size())
    {
        hz_depth_valid_[current_frame] = 1;
        gtao_slot_view_[current_frame] = camera_.view();
        gtao_slot_view_proj_[current_frame] = camera_.view_proj();
        gtao_slot_proj_[current_frame] =
            camera_.view_proj() * glm::inverse(camera_.view());
    }
}

// Brief 04d PHASE 2: recorded INSIDE the reopened MSAA group (which LOADed phase-1's color+depth).
// Renders the disocclusion complement (bit-clear meshlets that the fresh pyramid says are visible),
// sets their bits, then the sorted transparency pass (tests vs the final opaque depth, as before).
void GeometryPass::record_phase2(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    // Brief 11 step 2: this is now an ALWAYS-scheduled pass (geometry.phase2). In single-pass mode
    // record() already drew the disocclusion-free opaque + probe + transparency, so phase 2 must not
    // run — else it double-draws. Only the two-phase path uses it.
    if (!two_phase_active_ || !meshlet_program_ || !draw_info_mapped_) return;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    record_opaque_phase(recorder, current_frame, /*phase*/ 2u);
    // Brief 09b: probe-debug spheres after opaque, depth-tested. Brief 11 M2: transparency moved OUT to
    // the standalone TransparencyPass, which draws last in this same reopened MSAA group (byte-identical).
    record_probe_debug(recorder);
}

}  // namespace sandbox
