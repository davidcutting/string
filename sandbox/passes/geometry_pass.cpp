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

GeometryPass::GeometryPass(PassContext& context, std::vector<std::filesystem::path> model_paths,
                           std::shared_ptr<MeshOverlayStats> overlay_stats, bool lookdev)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, frames_in_flight_(context.frames_in_flight)
, residency_(kTextureBudget, kTextureStreamPerFrame)
, overlay_stats_(std::move(overlay_stats))
, gpu_profiler_ctx_(context.gpu_profiler_ctx)
{
    // Brief 04e M3: per-frame transient buffers (worklists, draw_lod) reserve into the
    // renderer's scratch arena; buffers bind lazily in update() after materialization.
    scratch_ = &context.scratch;
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

    // --- Procedural sky background ------------------------------------------------------------
    // A fullscreen pass drawn (in record()) before the geometry, into the same HDR target; no depth
    // test/write so opaque geometry overwrites it. Fills the void and drives the IBL ambient.
    // Procedural: no descriptor bindings, only a push constant — reflection reports zero sets and
    // the push-constant range. Built via the hot-reload registry (recompiles + swaps on save).
    VkSampleCountFlagBits sky_samples = context.sample_count;
    sky_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "sky_shader.slang",
        [sky_samples](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({})
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev);
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
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .set_multisampling(sky_samples)
                .enable_depth_stencil(false, false)   // background: declare depth format but don't test/write
                .enable_color_blending()
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

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
        scene_buffers_.resize(frames_in_flight_);
        scene_mapped_.resize(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            scene_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
                .size = sizeof(SceneData),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                  | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            });
            scene_mapped_[f] = allocator_.get_buffer(scene_buffers_[f]).allocation_info.pMappedData;
        }

        // --- Local lights: build the stress scene + per-frame light SSBO ring ---
        build_light_stress_scene(scene_aabb_min_, scene_aabb_max_);
        light_buffers_.resize(frames_in_flight_);
        light_mapped_.resize(frames_in_flight_);
        const VkDeviceSize light_capacity = sizeof(GpuLight) * std::max<std::size_t>(1, lights_.size());
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            light_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
                .size = light_capacity,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                  | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            });
            light_mapped_[f] = allocator_.get_buffer(light_buffers_[f]).allocation_info.pMappedData;
        }

        // --- Froxel index buffers (device-local, written by the binning compute) ---
        // Sized to the max grid at the largest expected resolution; reallocated lazily if the screen
        // grows past it. For 4K: 240x135x24 froxels * (1+128) uints ~= 100MB per frame — sized to a
        // 2560x1440 default here (160x90x24) and grown as needed in update().
        froxel_buffers_.resize(frames_in_flight_);
        // Actual allocation is deferred to ensure_froxel_capacity() from update() once screen_size is
        // known; leave the ids at 0 (created on first update).

        // --- Froxel light-binning compute pipeline (Slang, hot-reloadable) ---
        froxel_program_ = context.shader_registry.create(
            context.resources_path / "shaders" / "froxel_cull.slang",
            [](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                {
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                }
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = string::gpu::pipeline_type::COMPUTE;
                return p;
            });

        // Brief 07: dynamic sky IBL resources + pipelines (env cubemaps, SH buffer, DFG LUT).
        create_ibl_resources(context);

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
void GeometryPass::create_ibl_resources(PassContext& context)
{
    // Linear clamp-to-edge trilinear sampler: the prefilter ladder interpolates between roughness
    // mips, and the DFG LUT must not wrap at NdotV/roughness extremes (the allocator's default
    // sampler REPEATs).
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    if (vkCreateSampler(device_.get_device(), &sampler_info, nullptr, &env_sampler_) != VK_SUCCESS)
        throw std::runtime_error("GeometryPass: failed to create env sampler");

    const auto make_cube = [&](uint32_t mips) {
        return allocator_.create_resource(string::gpu::image_info{
            .extent = { kEnvSize, kEnvSize, 1 },
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
            .mip_levels = mips,
            .cube = true,
        });
    };
    env_capture_ = make_cube(kEnvCaptureMips);
    env_prefiltered_ = make_cube(kEnvPrefilterMips);

    // Sampled (SamplerCube) slots + per-mip 2D_ARRAY storage views for the compute writes.
    const auto bind_cube = [&](string::gpu::resource_id id, uint32_t mips,
                               std::vector<VkImageView>& views, std::vector<uint32_t>& slots) {
        const string::gpu::allocated_image& img = allocator_.get_image(id);
        descriptor_table_.bind(id, string::gpu::descriptor_type::TEXTURE);
        const uint32_t sample_slot =
            descriptor_table_.get_binding_slot(id, string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(sample_slot, img.view, env_sampler_);
        for (uint32_t m = 0; m < mips; ++m)
        {
            const VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = img.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 6 },
            };
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device_.get_device(), &vi, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("GeometryPass: failed to create env mip view");
            views.push_back(view);
            slots.push_back(descriptor_table_.bind_storage_view(view));
        }
        return sample_slot;
    };
    env_capture_sample_slot_ = bind_cube(env_capture_, kEnvCaptureMips,
                                         env_capture_mip_views_, env_capture_mip_slots_);
    env_prefiltered_slot_ = bind_cube(env_prefiltered_, kEnvPrefilterMips,
                                      env_prefiltered_mip_views_, env_prefiltered_mip_slots_);

    // DFG LUT: 2D RGBA16F (rg used), baked once; TRANSFER_SRC for the dbg.ibl_verify readback.
    dfg_lut_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { kDfgSize, kDfgSize, 1 },
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    const string::gpu::allocated_image& dfg = allocator_.get_image(dfg_lut_);
    descriptor_table_.bind(dfg_lut_, string::gpu::descriptor_type::TEXTURE);
    dfg_sample_slot_ = descriptor_table_.get_binding_slot(dfg_lut_, string::gpu::descriptor_type::TEXTURE);
    descriptor_table_.update_texture(dfg_sample_slot_, dfg.view, env_sampler_);
    dfg_storage_slot_ = descriptor_table_.bind_storage_view(dfg.view);

    // SH coefficients (9 x float4), written by the projection compute, read by every lit fragment.
    sh_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = sizeof(float) * 4 * 9,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // The five compute pipelines, one per ibl.slang entry point (hot-reload registry).
    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    const auto make_ibl_entry = [&](const char* entry) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / "ibl.slang",
            [layout, entry](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
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
    env_capture_program_ = make_ibl_entry("capture_main");
    env_mip_program_ = make_ibl_entry("mip_main");
    env_prefilter_program_ = make_ibl_entry("prefilter_main");
    sh_project_program_ = make_ibl_entry("sh_project_main");
    dfg_program_ = make_ibl_entry("dfg_main");

    STRING_LOG_INFO("[ibl] env {}px cube x{} mips (capture) / x{} mips (prefiltered ladder), "
                    "DFG {}px, L2 SH", kEnvSize, kEnvCaptureMips, kEnvPrefilterMips, kDfgSize);
}

// Record the sky-IBL update chain: capture -> capture mip chain -> SH projection + GGX prefilter
// ladder. Runs only on frames where the sun moved past the trigger (see update()) — the whole
// chain is a single-frame update, so the ambient is always self-consistent (no popping). The DFG
// LUT bake rides the first call. All barriers here are the documented INTRA-pass class (like the
// HiZ mip chain): everything is produced and consumed by this pass; the SH buffer's fragment-read
// edge is graph-declared (usages) and the final memory barrier makes the image writes visible to
// the fragment stage.
void GeometryPass::record_ibl_update(VkCommandBuffer cb)
{
    VkDescriptorSet set = descriptor_table_.get_set();
    const auto dispatch = [&](string::gpu::shader_program* prog, const IblPush& push,
                              uint32_t gx, uint32_t gy, uint32_t gz) {
        const string::gpu::pipeline& p = prog->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(IblPush), &push);
        vkCmdDispatch(cb, gx, gy, gz);
    };
    const auto compute_barrier = [&](VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        vkCmdPipelineBarrier2(cb, &dep);
    };

    IblPush push{};
    push.sun_dir = glm::vec4(glm::normalize(sun_dir_), furnace_ ? 1.0f : 0.0f);
    push.sky_zenith = glm::vec4(sky_zenith_, 0.0f);
    push.sky_ground = glm::vec4(sky_ground_, 0.0f);
    push.sun_color = glm::vec4(sun_color_, sun_intensity_);   // w: klx (ground-band lighting)
    push.sh = allocator_.get_buffer(sh_buffer_).device_address;

    // One-time: DFG LUT bake + move the cubemaps into their permanent GENERAL layout.
    if (!dfg_baked_)
    {
        const string::gpu::allocated_image& dfg = allocator_.get_image(dfg_lut_);
        vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        IblPush dpush = push;
        dpush.dst_slot = dfg_storage_slot_;
        dpush.dst_size = kDfgSize;
        dpush.sample_count = kDfgSamples;
        dispatch(dfg_program_, dpush, (kDfgSize + 7) / 8, (kDfgSize + 7) / 8, 1);
        vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_GENERAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        dfg_baked_ = true;
    }
    if (!ibl_layouts_initialized_)
    {
        for (string::gpu::resource_id id : { env_capture_, env_prefiltered_ })
        {
            const string::gpu::allocated_image& img = allocator_.get_image(id);
            vku::transition_image(cb, {
                .image = img.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .new_layout = VK_IMAGE_LAYOUT_GENERAL,
                .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
                .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                            | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .level_count = img.mip_levels,
                .layer_count = 6,
            });
        }
        ibl_layouts_initialized_ = true;
    }
    else
    {
        // Cross-frame WAR: last frame's fragment reads of the prefiltered ladder must retire
        // before this frame's rewrite (execution dependency; no memory flush needed for reads).
        const VkMemoryBarrier2 war = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &war };
        vkCmdPipelineBarrier2(cb, &dep);
    }

    // 1) Sky -> capture mip 0.
    {
        IblPush cpush = push;
        cpush.dst_slot = env_capture_mip_slots_[0];
        cpush.dst_size = kEnvSize;
        dispatch(env_capture_program_, cpush, kEnvSize / 8, kEnvSize / 8, 6);
    }
    // 2) Capture average chain (PDF-mip source + SH source).
    for (uint32_t m = 1; m < kEnvCaptureMips; ++m)
    {
        compute_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        IblPush mpush = push;
        mpush.src_slot = env_capture_mip_slots_[m - 1];
        mpush.dst_slot = env_capture_mip_slots_[m];
        mpush.src_size = kEnvSize >> (m - 1);
        mpush.dst_size = kEnvSize >> m;
        dispatch(env_mip_program_, mpush, (mpush.dst_size + 7) / 8, (mpush.dst_size + 7) / 8, 6);
    }
    // Capture writes -> SH storage reads + prefilter SAMPLED reads.
    compute_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 3) L2 SH projection (one workgroup; the fragment-read edge is graph-declared in usages).
    {
        IblPush spush = push;
        spush.src_slot = env_capture_mip_slots_[kShSourceMip];
        spush.src_size = kEnvSize >> kShSourceMip;
        dispatch(sh_project_program_, spush, 1, 1, 1);
    }
    // 4) GGX prefilter ladder (mips independent — no barriers between them).
    for (uint32_t m = 0; m < kEnvPrefilterMips; ++m)
    {
        IblPush ppush = push;
        ppush.src_slot = env_capture_sample_slot_;
        ppush.src_size = kEnvSize;
        ppush.dst_slot = env_prefiltered_mip_slots_[m];
        ppush.dst_size = kEnvSize >> m;
        ppush.roughness = float(m) / float(kEnvPrefilterMips - 1);
        ppush.sample_count = kPrefilterSamples;
        ppush.mip_count = kEnvCaptureMips;
        dispatch(env_prefilter_program_, ppush, (ppush.dst_size + 7) / 8, (ppush.dst_size + 7) / 8, 6);
    }
    // Ladder writes -> the lit fragments' sampled reads (image stays in GENERAL).
    compute_barrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    ibl_captured_sun_dir_ = glm::normalize(sun_dir_);
    ibl_captured_furnace_ = furnace_;
    ibl_primed_ = true;
    ++ibl_update_count_;
}

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

void GeometryPass::create_probe_resources(PassContext& context)
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
        const string::gpu::resource_id id = allocator_.create_resource(string::gpu::image_info{
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
        const string::gpu::allocated_image& img = allocator_.get_image(id);
        descriptor_table_.bind(id, string::gpu::descriptor_type::TEXTURE);
        sample_slot = descriptor_table_.get_binding_slot(id, string::gpu::descriptor_type::TEXTURE);
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
            [layout, entry](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
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
    probe_clear_program_ = make_probe_compute("probe_capture.slang", "clear_main");
    probe_collapse_program_ = make_probe_compute("probe_capture.slang", "collapse_main");
    probe_relight_program_ = make_probe_compute("probe_relight.slang", "relight_main");

    // Cube G-buffer (albedo + normal/dist + depth), reused across probes within a frame. All 6 faces
    // render in ONE multiview pass, so each image gets a single 6-layer 2D_ARRAY view (the render
    // target, viewMask=0x3F) + a SamplerCube read slot for collapse.
    const auto make_gbuf_cube = [&](VkFormat fmt, VkImageAspectFlags aspect, VkImageUsageFlags usage,
                                    VkImageView& array_view, uint32_t* sample_slot) {
        const string::gpu::resource_id id = allocator_.create_resource(string::gpu::image_info{
            .extent = { kProbeCubeFace, kProbeCubeFace, 1 },
            .format = fmt,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .aspect_flags = aspect,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
            .cube = true,
        });
        const string::gpu::allocated_image& img = allocator_.get_image(id);
        if (sample_slot != nullptr)   // SamplerCube read slot (albedo / normal-dist only; not depth)
        {
            descriptor_table_.bind(id, string::gpu::descriptor_type::TEXTURE);
            *sample_slot = descriptor_table_.get_binding_slot(id, string::gpu::descriptor_type::TEXTURE);
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
    probe_active_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = std::max<VkDeviceSize>(sizeof(uint32_t) * probe_volume_.total(), sizeof(uint32_t)),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Per-probe relocation offset (RTXGI): float4 per probe (xyz world offset). Written by the collapse;
    // consumed via probe_common probe_world (relight bounce / M3 shading) + the debug spheres.
    probe_offset_ = allocator_.create_resource(string::gpu::buffer_info{
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
        probe_meshlet_draw_ = allocator_.create_resource(string::gpu::buffer_info{
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
        [layout](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev);
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
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // Debug-sphere graphics pipeline (instanced, procedural sphere; MSAA + depth-test, no blend).
    const VkSampleCountFlagBits scene_samples = context.sample_count;
    probe_debug_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "probe_debug.slang",
        [layout, scene_samples](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::GRAPHICS);
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
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
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
void GeometryPass::record_probe_capture(VkCommandBuffer cb)
{
    if (!probe_volume_.valid || probe_clear_program_ == nullptr || probe_capture_raster_program_ == nullptr
        || probe_collapse_program_ == nullptr || draw_info_mapped_ == nullptr)
        return;
    VkDescriptorSet set = descriptor_table_.get_set();
    const uint32_t total = probe_volume_.total();
    const float far_distance = glm::length(scene_aabb_max_ - scene_aabb_min_);
    const auto t0 = std::chrono::steady_clock::now();

    const auto counts = probe_volume_.counts;

    // --- One-time init: atlas layouts + clear_main over all probes + zero activation ---------------
    if (!probe_layouts_initialized_)
    {
        for (string::gpu::resource_id id : { probe_irrad_, probe_cap_gbuf_, probe_cap_albedo_, probe_vis_ })
        {
            const string::gpu::allocated_image& img = allocator_.get_image(id);
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
            const string::gpu::allocated_image& irr = allocator_.get_image(probe_irrad_);
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
        const string::gpu::pipeline& clr = probe_clear_program_->current();
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

    const string::gpu::allocated_image& cube_alb = allocator_.get_image(probe_cube_albedo_);
    const string::gpu::allocated_image& cube_nd = allocator_.get_image(probe_cube_nd_);
    const string::gpu::allocated_image& cube_dep = allocator_.get_image(probe_cube_depth_);
    const VkDeviceAddress active_addr = allocator_.get_buffer(probe_active_).device_address;

    // --- Capture, GPU-driven multiview, amortized by PROBE -----------------------------------------
    // All 6 cube faces render in ONE BeginRendering (multiview viewMask=0x3F) over 6-layer array
    // views; the mesh shader picks the per-face view-projection by SV_ViewID. The dispatch is a SINGLE
    // flat vkCmdDrawMeshTasksEXT over all LOD0 meshlets — the task shader resolves each flat index to
    // {draw, meshlet} via the load-time table, GPU frustum-culls against all 6 faces, and amplifies
    // only survivors. NO CPU per-draw loop, NO CPU culling. We capture kProbesPerFrame probes/frame,
    // so a ~250-probe bake finishes in ~20 frames (a couple seconds) with no visible hitch.
    const string::gpu::pipeline& rp = probe_capture_raster_program_->current();
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
            for (const string::gpu::allocated_image* img : { &cube_alb, &cube_nd })
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
        for (const string::gpu::allocated_image* img : { &cube_alb, &cube_nd })
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
        const string::gpu::pipeline& col = probe_collapse_program_->current();
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
void GeometryPass::record_probe_relight(VkCommandBuffer cb, uint16_t current_frame)
{
    if (!probe_volume_.valid || probe_relight_program_ == nullptr || sh_buffer_ == 0
        || current_frame >= scene_buffers_.size() || scene_buffers_[current_frame] == 0)
        return;
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
    push.sh = allocator_.get_buffer(sh_buffer_).device_address;
    push.active = allocator_.get_buffer(probe_active_).device_address;
    push.offsets = allocator_.get_buffer(probe_offset_).device_address;
    push.scene = allocator_.get_buffer(scene_buffers_[current_frame]).device_address;

    const string::gpu::pipeline& p = probe_relight_program_->current();
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
void GeometryPass::record_probe_debug(VkCommandBuffer cb)
{
    if (!probe_volume_.valid || probe_debug_program_ == nullptr || probe_debug_mode_ == 0) return;
    VkDescriptorSet set = descriptor_table_.get_set();
    const string::gpu::pipeline& p = probe_debug_program_->current();

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

namespace
{

float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? std::numeric_limits<float>::quiet_NaN()
                               : std::numeric_limits<float>::infinity();
    else v = std::ldexp(static_cast<float>(man + 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// CPU reference for the split-sum DFG integral — the same estimator (Hammersley + GGX importance
// sampling + height-correlated Smith visibility) as dfg_main in ibl.slang, in double precision.
glm::dvec2 dfg_reference(double NdotV, double perceptual, uint32_t samples)
{
    const double alpha = perceptual * perceptual;
    const glm::dvec3 V(std::sqrt(std::max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);
    double a = 0.0, b = 0.0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        uint32_t bits = i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
        bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
        bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
        const glm::dvec2 xi(double(i) / samples, double(bits) * 2.3283064365386963e-10);
        const double phi = 2.0 * glm::pi<double>() * xi.x;
        const double ct = std::sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        const double st = std::sqrt(std::max(1.0 - ct * ct, 0.0));
        const glm::dvec3 H(st * std::cos(phi), st * std::sin(phi), ct);
        const glm::dvec3 L = 2.0 * glm::dot(V, H) * H - V;
        if (L.z <= 0.0) continue;
        const double NdotL = L.z;
        const double NdotH = std::max(H.z, 0.0);
        const double VdotH = std::max(glm::dot(V, H), 1e-4);
        const double a2 = alpha * alpha;
        const double gv = NdotL * std::sqrt(NdotV * NdotV * (1.0 - a2) + a2);
        const double gl = NdotV * std::sqrt(NdotL * NdotL * (1.0 - a2) + a2);
        const double vis = 0.5 / std::max(gv + gl, 1e-7);
        const double g_vis = 4.0 * vis * VdotH * NdotL / std::max(NdotH, 1e-4);
        const double fc = std::pow(1.0 - VdotH, 5.0);
        a += (1.0 - fc) * g_vis;
        b += fc * g_vis;
    }
    return { a / samples, b / samples };
}

}  // namespace

// dbg.ibl_verify (brief 07 M1 numeric gate): read the DFG LUT + SH coefficients back and check
// them against references. PASS criteria: (a) DFG matches the CPU double-precision integral at
// probe points within 0.02 and A+B stays in (0, 1.01] (single-scatter albedo can't exceed 1);
// (b) under r.furnace the SH DC reconstructs E/pi = 1 +- 0.02 with all higher bands ~0; without
// the furnace, reconstructed sky irradiance is finite and up > down (sky brighter than ground).
void GeometryPass::run_ibl_verification()
{
    if (sh_buffer_ == 0 || dfg_lut_ == 0) return;
    vkDeviceWaitIdle(device_.get_device());

    const VkDeviceSize sh_bytes = sizeof(float) * 4 * 9;
    const VkDeviceSize dfg_bytes = VkDeviceSize(kDfgSize) * kDfgSize * 8;   // RGBA16F
    const string::gpu::resource_id staging = allocator_.create_resource(string::gpu::buffer_info{
        .size = sh_bytes + dfg_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    {
        string::gpu::command_recorder rec;
        rec.init(device_.get_device(), device_.get_queue(string::gpu::queue_type::GRAPHICS));
        VkCommandBuffer cb = rec.begin();
        const VkBufferCopy sh_region = { 0, 0, sh_bytes };
        vkCmdCopyBuffer(cb, allocator_.get_buffer(sh_buffer_).buffer,
                        allocator_.get_buffer(staging).buffer, 1, &sh_region);
        const string::gpu::allocated_image& dfg = allocator_.get_image(dfg_lut_);
        vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        const VkBufferImageCopy dfg_region = {
            .bufferOffset = sh_bytes,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { kDfgSize, kDfgSize, 1 },
        };
        vkCmdCopyImageToBuffer(cb, dfg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               allocator_.get_buffer(staging).buffer, 1, &dfg_region);
        vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        rec.end().immediate_submit();
        rec.destroy();
    }

    const uint8_t* mapped = static_cast<const uint8_t*>(
        allocator_.get_buffer(staging).allocation_info.pMappedData);
    const float* sh = reinterpret_cast<const float*>(mapped);
    const uint16_t* dfg = reinterpret_cast<const uint16_t*>(mapped + sh_bytes);

    bool pass = true;

    // --- SH checks --------------------------------------------------------------------------
    const auto sh_c = [&](int i) { return glm::vec3(sh[i * 4 + 0], sh[i * 4 + 1], sh[i * 4 + 2]); };
    const auto e_over_pi = [&](const glm::vec3& n) {
        return sh_c(0) * 0.282095f
             + sh_c(1) * (0.488603f * n.y) + sh_c(2) * (0.488603f * n.z) + sh_c(3) * (0.488603f * n.x)
             + sh_c(4) * (1.092548f * n.x * n.y) + sh_c(5) * (1.092548f * n.y * n.z)
             + sh_c(6) * (0.315392f * (3.0f * n.z * n.z - 1.0f))
             + sh_c(7) * (1.092548f * n.x * n.z)
             + sh_c(8) * (0.546274f * (n.x * n.x - n.y * n.y));
    };
    if (furnace_)
    {
        const float dc = sh_c(0).r * 0.282095f;
        float residual = 0.0f;
        for (int i = 1; i < 9; ++i)
            residual = std::max(residual, std::max(std::abs(sh_c(i).r),
                        std::max(std::abs(sh_c(i).g), std::abs(sh_c(i).b))));
        const bool ok = std::abs(dc - 1.0f) < 0.02f && residual < 0.02f;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] furnace SH: DC E/pi = {:.5f} (expect 1.0), max |l>0| = {:.5f} -> {}",
                        dc, residual, ok ? "PASS" : "FAIL");
    }
    else
    {
        const glm::vec3 up = e_over_pi(glm::vec3(0, 1, 0));
        const glm::vec3 down = e_over_pi(glm::vec3(0, -1, 0));
        const bool ok = std::isfinite(up.r + up.g + up.b) && up.g > down.g && down.g >= -0.05f;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] sky SH: E/pi(+Y) = ({:.3f},{:.3f},{:.3f}), E/pi(-Y) = "
                        "({:.3f},{:.3f},{:.3f}) -> {}",
                        up.r, up.g, up.b, down.r, down.g, down.b, ok ? "PASS" : "FAIL");
    }

    // --- DFG checks -------------------------------------------------------------------------
    const auto dfg_at = [&](uint32_t x, uint32_t y) {
        const uint16_t* t = dfg + (VkDeviceSize(y) * kDfgSize + x) * 4;
        return glm::vec2(half_to_float(t[0]), half_to_float(t[1]));
    };
    float sum_max = 0.0f, sum_min = 10.0f;
    for (uint32_t y = 0; y < kDfgSize; ++y)
        for (uint32_t x = 0; x < kDfgSize; ++x)
        {
            const glm::vec2 v = dfg_at(x, y);
            sum_max = std::max(sum_max, v.x + v.y);
            sum_min = std::min(sum_min, v.x + v.y);
        }
    const bool bounded = sum_max <= 1.01f && sum_min > 0.0f;
    pass = pass && bounded;
    STRING_LOG_INFO("[ibl-verify] DFG A+B range [{:.4f}, {:.4f}] (expect (0, 1.01]) -> {}",
                    sum_min, sum_max, bounded ? "PASS" : "FAIL");
    const glm::vec2 probes[5] = { { 0.5f, 0.5f }, { 0.9f, 0.1f }, { 0.2f, 0.8f },
                                  { 0.7f, 0.3f }, { 0.95f, 0.95f } };
    for (const glm::vec2& p : probes)
    {
        const uint32_t x = std::min(kDfgSize - 1, uint32_t(p.x * kDfgSize));
        const uint32_t y = std::min(kDfgSize - 1, uint32_t(p.y * kDfgSize));
        const double nv = (x + 0.5) / kDfgSize;
        const double r = (y + 0.5) / kDfgSize;
        const glm::vec2 gpu = dfg_at(x, y);
        const glm::dvec2 ref = dfg_reference(nv, r, kDfgSamples);
        const bool ok = std::abs(gpu.x - ref.x) < 0.02 && std::abs(gpu.y - ref.y) < 0.02;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] DFG({:.2f},{:.2f}): gpu ({:.4f},{:.4f}) ref ({:.4f},{:.4f}) -> {}",
                        nv, r, gpu.x, gpu.y, ref.x, ref.y, ok ? "PASS" : "FAIL");
    }

    STRING_LOG_INFO("[ibl-verify] overall: {}", pass ? "PASS" : "FAIL");
    allocator_.destroy_resource(staging);
}

// Upload the meshlet heaps, build the DrawInfo table (materials + transforms + bounds + LOD ranges),
// allocate the visibility bitfield + stats ring, and create the task/mesh/HiZ/reset pipelines.
void GeometryPass::build_meshlet_gpu(PassContext& context)
{
    if (meshlet_model_.total_meshlets == 0) return;

    auto make_device_buffer = [&](const void* data, VkDeviceSize size, VkBufferUsageFlags extra) {
        string::gpu::resource_id id = allocator_.create_resource(string::gpu::buffer_info{
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
    draw_info_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = draw_info_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    draw_info_mapped_ = static_cast<GpuDrawInfo*>(
        allocator_.get_buffer(draw_info_buffer_).allocation_info.pMappedData);
    std::copy(meshlet_model_.draws.begin(), meshlet_model_.draws.end(), draw_info_mapped_);

    // GPU-written stats, read back one frame late (host-visible ring).
    stats_buffers_.resize(frames_in_flight_);
    stats_readback_.resize(frames_in_flight_);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        stats_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
            .size = sizeof(GpuMeshStats),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        });
    }

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
    visbits_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = VkDeviceSize(u32) * std::max(1u, visbits_words_),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Per-draw LOD selected LAST frame (for the LOD-switch bit invalidation). Zeroed at first clear.
    prev_draw_lod_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
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
        transp_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
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
        return [layout, samples, cull_mode, depth_write](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev);
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
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
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
        [layout](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            p.push_constants = compiled.layout.push_constant;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ layout })
                .set_push_constant_ranges({ compiled.layout.push_constant })
                .build(dev);
            string::gpu::pipeline_builder builder(dev);
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
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // HiZ pyramid downsample + stats reset compute pipelines.
    auto make_compute = [&](const char* file) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / file,
            [layout](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = string::gpu::pipeline_type::COMPUTE;
                return p;
            });
    };
    // Brief 04c: the expansion shader has three compute entry points (scan_blocks / scan_carry /
    // fill). Select one by name so each becomes its own pipeline through the same hot-reload registry.
    auto make_compute_entry = [&](const char* file, const char* entry) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / file,
            [layout, entry](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
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

// Build (or tear down) the crowd stress scene: duplicate every base draw across a kCrowdGrid x
// kCrowdGrid grid of translated copies, to prove 500+-crowd geometry throughput (brief M6). Crowd
// draws are extra DrawInfo entries referencing the SAME meshlet buffers — only the transform (and
// thus the world-space bounds) differ. Visibility bits are shared with the base draws (conservative
// for the shared meshlet ids — fine for a throughput stress test).
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

// (Re)create the HiZ pyramid for the current screen size if it changed. Power-of-two conservative
// sizing (each mip is half, rounded down, min 1); R32F mip chain. Binds the whole-chain sampled slot
// (binding 1) + a storage-image slot per mip (binding 2) for the downsample compute.
void GeometryPass::ensure_hiz(uint16_t current_frame)
{
    if (screen_size.width == 0 || screen_size.height == 0) return;
    // Conservative power-of-two base covering the screen (so a min-reduce never misses a texel).
    auto next_pow2 = [](uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    const uint32_t base_w = next_pow2(screen_size.width) / 2;   // half-res mip0 is plenty for HiZ
    const uint32_t base_h = next_pow2(screen_size.height) / 2;
    if (base_w == hiz_screen_w_ && base_h == hiz_screen_h_ && hiz_[current_frame].image != 0) return;
    hiz_screen_w_ = base_w;
    hiz_screen_h_ = base_h;

    const uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(std::max(base_w, base_h)))) + 1;

    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        HizPyramid& hz = hiz_[f];
        // Retire the old pyramid's slots/views/image.
        for (uint32_t s : hz.mip_storage_slots) descriptor_table_.unbind_storage_view(s);
        for (VkImageView v : hz.mip_views) vkDestroyImageView(device_.get_device(), v, nullptr);
        if (hz.image != 0)
        {
            descriptor_table_.unbind(hz.image, string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(hz.image);
        }
        if (hz.depth != 0)
        {
            descriptor_table_.unbind(hz.depth, string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(hz.depth);
        }
        hz = HizPyramid{};

        hz.mips = mips;
        hz.size = glm::uvec2(base_w, base_h);

        // Single-sample D32 depth for the camera prepass the pyramid reduces from (full screen res).
        hz.depth = allocator_.create_resource(string::gpu::image_info{
            .extent = { screen_size.width, screen_size.height, 1 },
            .format = VK_FORMAT_D32_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        descriptor_table_.bind(hz.depth, string::gpu::descriptor_type::TEXTURE);
        hz.depth_slot = descriptor_table_.get_binding_slot(hz.depth, string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(hz.depth_slot, allocator_.get_image(hz.depth).view, hiz_sampler_);
        hz.image = allocator_.create_resource(string::gpu::image_info{
            .extent = { base_w, base_h, 1 },
            .format = VK_FORMAT_R32_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
            .mip_levels = mips,
        });
        const string::gpu::allocated_image& img = allocator_.get_image(hz.image);

        // Whole-chain sampled slot (for SampleLevel in the task shader).
        descriptor_table_.bind(hz.image, string::gpu::descriptor_type::TEXTURE);
        hz.sample_slot = descriptor_table_.get_binding_slot(hz.image, string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(hz.sample_slot, img.view, hiz_sampler_);

        // Per-mip storage views + slots (downsample writes mip N+1 while sampling mip N).
        for (uint32_t m = 0; m < mips; ++m)
        {
            VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = img.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R32_SFLOAT,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 },
            };
            VkImageView view = VK_NULL_HANDLE;
            vkCreateImageView(device_.get_device(), &vi, nullptr, &view);
            hz.mip_views.push_back(view);
            hz.mip_storage_slots.push_back(descriptor_table_.bind_storage_view(view));
        }
    }
    STRING_LOG_INFO("[hiz] pyramid {}x{}, {} mips (x{} frames)", base_w, base_h, mips, frames_in_flight_);
    // Brief 09: the hz.depth images were just recreated — every slot's resolved-depth content
    // (GTAO's input) is gone.
    std::fill(hz_depth_valid_.begin(), hz_depth_valid_.end(), uint8_t(0));
}

// --- Brief 09: GTAO targets ------------------------------------------------------------------------
// Half-res RGBA8 (rgb = world bent normal, a = visibility): one shared raw target (produced and
// denoised within one record hook) + one final target per frame slot (this frame's fragments
// sample it while the next frame's chain rewrites its own slot).
void GeometryPass::ensure_gtao()
{
    if (screen_size.width == 0 || screen_size.height == 0) return;
    const glm::uvec2 size((screen_size.width + 1) / 2, (screen_size.height + 1) / 2);
    if (gtao_raw_ != 0 && size == gtao_size_) return;
    gtao_size_ = size;

    const auto drop_image = [&](string::gpu::resource_id& id) {
        if (id == 0) return;
        descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(id);
        id = 0;
    };
    if (gtao_raw_storage_slot_ != UINT32_MAX)
        descriptor_table_.unbind_storage_view(gtao_raw_storage_slot_);
    for (uint32_t s : gtao_final_storage_slots_) descriptor_table_.unbind_storage_view(s);
    gtao_final_storage_slots_.clear();
    gtao_final_sampled_slots_.clear();
    drop_image(gtao_raw_);
    for (string::gpu::resource_id& id : gtao_final_) drop_image(id);
    gtao_final_.assign(frames_in_flight_, 0);
    gtao_final_ready_.assign(frames_in_flight_, 0);
    gtao_raw_initialized_ = false;

    const auto make_target = [&](uint32_t& sampled_slot, uint32_t& storage_slot) {
        // RGBA16F, not RGBA8: 8-bit visibility quantizes into wide soft bands on smooth
        // slowly-curving receivers (the Sponza vaults — 1/255 vis steps multiply straight into
        // the ambient term). Half-res 16F is ~4 B/px extra; the bent normal rides along at the
        // higher precision for free.
        const string::gpu::resource_id id = allocator_.create_resource(string::gpu::image_info{
            .extent = { size.x, size.y, 1 },
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        const string::gpu::allocated_image& img = allocator_.get_image(id);
        descriptor_table_.bind(id, string::gpu::descriptor_type::TEXTURE);
        sampled_slot = descriptor_table_.get_binding_slot(id, string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(sampled_slot, img.view, gtao_sampler_);
        storage_slot = descriptor_table_.bind_storage_view(img.view);
        return id;
    };
    gtao_raw_ = make_target(gtao_raw_sampled_slot_, gtao_raw_storage_slot_);
    gtao_final_sampled_slots_.resize(frames_in_flight_);
    gtao_final_storage_slots_.resize(frames_in_flight_);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
        gtao_final_[f] = make_target(gtao_final_sampled_slots_[f], gtao_final_storage_slots_[f]);
    STRING_LOG_INFO("[gtao] half-res targets {}x{} (raw + {} final slots)", size.x, size.y,
                    frames_in_flight_);
}

// Record the GTAO chain (top of record_compute): horizon-search AO + bent normal from the
// PREVIOUS slot's resolved depth, then the spatial denoise into this slot's final target.
// Barriers are the documented cross-frame local class (07 precedent: same-queue, cross-CB;
// hz.depth is untracked pass-managed state, its resolve re-discards from UNDEFINED).
void GeometryPass::record_gtao(VkCommandBuffer cb, uint16_t current_frame)
{
    const uint16_t prev = (current_frame + frames_in_flight_ - 1) % frames_in_flight_;
    if (prev >= hiz_.size() || hiz_[prev].depth == 0 || gtao_raw_ == 0) return;
    const HizPyramid& hz = hiz_[prev];
    const string::gpu::allocated_image& depth = allocator_.get_image(hz.depth);
    const string::gpu::allocated_image& raw = allocator_.get_image(gtao_raw_);
    const string::gpu::allocated_image& fin = allocator_.get_image(gtao_final_[current_frame]);

    STRING_PROFILE_GPU_ZONE(gpu_ctx(), cb, "gtao")

    // Prev-frame resolved depth: DEPTH_ATTACHMENT (where record_between parked it) -> sampled.
    // Left in SHADER_READ_ONLY afterwards — the renderer's next resolve of this slot re-discards
    // from UNDEFINED with a src scope that already names COMPUTE sampled reads.
    vku::transition_image(cb, {
        .image = depth.image,
        .old_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                   | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
    });
    if (!gtao_raw_initialized_)
    {
        gtao_raw_initialized_ = true;
        vku::transition_image(cb, {
            .image = raw.image,
            .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }
    else
    {
        vku::transition_image(cb, {
            .image = raw.image,
            .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }
    vku::transition_image(cb, {
        .image = fin.image,
        .old_layout = gtao_final_ready_[current_frame] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .src_stage = gtao_final_ready_[current_frame]
            ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .src_access = gtao_final_ready_[current_frame]
            ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VkAccessFlags2(0),
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });

    const glm::mat4& proj = gtao_slot_proj_[prev];
    GtaoPush push{};
    push.view = gtao_slot_view_[prev];
    push.inv_proj = glm::inverse(proj);
    push.depth_slot = hz.depth_slot;
    push.dst_slot = gtao_raw_storage_slot_;
    push.dst_size = gtao_size_;
    push.depth_size = glm::uvec2(screen_size.width, screen_size.height);
    push.radius = std::max(cv_gtao_radius().get(), 0.01f);
    push.proj00 = std::abs(proj[0][0]);
    push.proj11 = std::abs(proj[1][1]);

    VkDescriptorSet set = descriptor_table_.get_set();
    const auto dispatch = [&](string::gpu::shader_program* prog, const GtaoPush& p_push) {
        const string::gpu::pipeline& p = prog->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GtaoPush), &p_push);
        vkCmdDispatch(cb, (gtao_size_.x + 7) / 8, (gtao_size_.y + 7) / 8, 1);
    };
    dispatch(gtao_program_, push);

    // raw writes -> denoise sampled reads.
    vku::transition_image(cb, {
        .image = raw.image,
        .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    GtaoPush denoise = push;
    denoise.src_slot = gtao_raw_sampled_slot_;
    denoise.dst_slot = gtao_final_storage_slots_[current_frame];
    dispatch(gtao_denoise_program_, denoise);

    // Final -> sampled for this frame's lit fragments.
    vku::transition_image(cb, {
        .image = fin.image,
        .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    gtao_final_ready_[current_frame] = 1;
}

// Brief 03b: dispatch the GPU draw-cull compute to build a work list (commands[] + records[] + count)
// for one consumer. `prepass` forces LOD0 (and skips the LOD histogram) for the HiZ depth prepass;
// the camera list (prepass=false) selects LODs and writes the histogram, and is reused by the main
// pass + all 3 shadow cascades. The list buffer's count is zeroed here, then the compute appends.
// Leaves a barrier making commands[]/records[]/count visible to DRAW_INDIRECT + task-shader reads.
void GeometryPass::record_draw_cull(VkCommandBuffer cb, uint16_t current_frame, int cascade)
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

    const string::gpu::pipeline& cp = draw_cull_program_->current();
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
    cull.stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address;
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
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp.pipeline);
    vkCmdPushConstants(cb, cp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(DrawCullPush), &cull);
    vkCmdDispatch(cb, (active_draw_count_ + 63) / 64, 1, 1);

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
    vkCmdPipelineBarrier2(cb, &dep);
}

// Brief 04c COMPACTION PHASE for one worklist: scan_blocks -> scan_carry -> fill, then a barrier so the
// resulting commands[] + records[] + count are visible to the task shader / indirect draw. The three
// passes are separated by storage barriers (each reads the previous pass's writes). The fill copies the
// per-draw selected LOD from `draw_lod` into each surviving draw's record.
void GeometryPass::record_expand(VkCommandBuffer cb, const Worklist& wl, VkDeviceAddress draw_lod)
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
        vkCmdPipelineBarrier2(cb, &dep);
    };

    const auto dispatch = [&](string::gpu::shader_program* prog, uint32_t groups) {
        const string::gpu::pipeline& p = prog->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ExpandPush), &push);
        vkCmdDispatch(cb, groups, 1, 1);
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
    vkCmdPipelineBarrier2(cb, &dep);
}

// Brief 04: build the sorted transparency work list on the CPU. BLEND draws (excluded from the opaque
// GPU lists) are sorted BACK-TO-FRONT by their world-space bounding-sphere distance to the eye, then
// written as a compacted commands[] + records[] + count into the host-visible per-frame buffer. v1
// uses LOD0 for every transparent draw (counts are small; per-draw sort only — no per-meshlet/OIT).
// Returns the number of draws written (indirect count). Runs on the render thread (host-visible write).
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

// Brief 04: draw the sorted transparency list (after opaque + sky). Same push as the main pass but
// blend_pass=1 (fragment outputs base-color alpha) via the transparency pipeline (blend on, depth
// test vs opaque depth, no depth write). HiZ stays valid (occlusion vs opaque depth is fine).
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

// Brief 04c (resolution b): the main pass draws the compacted camera list with ONE
// vkCmdDrawMeshTasksIndirectCountEXT. The compaction phase wrote the DENSE commands[] + records[] +
// count into `wl` (one command per surviving draw, ascending draw order); the task shader reads its
// {draw_index, lod} via SV_DrawIndex. groupCountX per command = ceil(LOD meshlets / 32). Command-index
// ordering across the draws is what pins coplanar depth-tie winners run-to-run (AE=0).
void GeometryPass::record_meshlet_draws(VkCommandBuffer cb, const string::gpu::pipeline& p,
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
    push.scene = allocator_.get_buffer(scene_buffers_[current_frame]).device_address;
    push.stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address;
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

    vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    vkCmdDrawMeshTasksIndirectCountEXT(cb, buf, wl.offset + wl_commands_off_, buf, wl.offset + wl_count_off_,
                                       cull_max_draws_, sizeof(uint32_t) * 3);
}

// Brief 04e M4: froxel light binning — one thread per froxel bins the local lights into
// per-froxel index lists the lit fragment shader reads. DEPENDENCY-FREE within the frame (reads
// only the host-written light SSBO ring), so the renderer places it on an async compute lane
// when the hardware exposes one (timeline edge + queue-family ownership transfer derived from
// async_usages), or records it inline on the main queue otherwise. No Tracy zone here: the
// renderer wraps the whole async chain in the LANE's own GPU context (a pass-side zone would
// use the main-queue context and produce bogus timestamps on the async queue).
bool GeometryPass::has_async_compute() const
{
    return froxel_program_ != nullptr && froxel_count_ > 0 && !froxel_buffers_.empty();
}

void GeometryPass::record_async_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (froxel_program_ == nullptr || froxel_count_ == 0 || current_frame >= froxel_buffers_.size()
        || froxel_buffers_[current_frame] == 0)
        return;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    const uint32_t light_count = lights_enabled_ ? static_cast<uint32_t>(lights_.size()) : 0u;
    const string::gpu::pipeline& fp = froxel_program_->current();
    glm::mat4 proj = camera_.view_proj() * glm::inverse(camera_.view());  // == projection
    const FroxelPush fpush{
        .view = camera_.view(),
        .inv_proj = glm::inverse(proj),
        .screen = glm::uvec2(screen_size.width, screen_size.height),
        .grid = glm::uvec2(froxel_tiles_x_, froxel_tiles_y_),
        .slices = kFroxelDepthSlices,
        .tile_size = kFroxelTileSize,
        .near_plane = camera_.near_plane(),
        .far_plane = std::min(settings_.shadow_depth_range, camera_.far_plane()),
        .light_count = light_count,
        .max_per_froxel = kMaxLightsPerFroxel,
        ._pad0 = 0, ._pad1 = 0,
        .lights = light_count > 0
                ? allocator_.get_buffer(light_buffers_[current_frame]).device_address : 0,
        .froxels = allocator_.get_buffer(froxel_buffers_[current_frame]).device_address,
    };
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, fp.pipeline);
    vkCmdPushConstants(command_buffer, fp.pipeline_layout, fp.push_constants.stageFlags,
                       0, sizeof(FroxelPush), &fpush);
    vkCmdDispatch(command_buffer, (froxel_tiles_x_ + 3) / 4, (froxel_tiles_y_ + 3) / 4,
                  (kFroxelDepthSlices + 3) / 4);
}

void GeometryPass::compute_cascades()
{
    // Practical split scheme (Zhang): blend a logarithmic and a uniform split by cascade_split_lambda,
    // over [near, shadow_depth_range]. Each cascade's ortho box is fit to that view-space depth slice's
    // frustum corners, then STABILIZED: the box is sized to a sphere (rotation-invariant so the sun
    // moving doesn't resize it) and its origin snapped to whole shadow texels (so neither the sun nor
    // the camera moving shimmers the shadow edges).
    const uint32_t count = std::min(settings_.cascade_count, kMaxCascades);
    const float near_clip = camera_.near_plane();
    const float far_clip = std::min(settings_.shadow_depth_range, camera_.far_plane());
    const float range = far_clip - near_clip;

    const glm::mat4 inv_cam = glm::inverse(camera_.view_proj());
    const glm::vec3 L = glm::normalize(sun_dir_);   // direction TO the sun

    float last_split = near_clip;
    for (uint32_t c = 0; c < count; ++c)
    {
        const float p = static_cast<float>(c + 1) / static_cast<float>(count);
        const float log_split = near_clip * std::pow(far_clip / near_clip, p);
        const float uniform_split = near_clip + range * p;
        const float split = settings_.cascade_split_lambda * log_split
                          + (1.0f - settings_.cascade_split_lambda) * uniform_split;
        cascade_split_[c] = split;

        // The 8 corners of this cascade's sub-frustum, mapped from NDC (reverse-Z: near=1, far=0) into
        // world via the inverse camera view-projection, then re-scaled so its near/far match this
        // slice's [last_split, split] view-space depths.
        glm::vec3 corners[8];
        int ci = 0;
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z)
                {
                    const glm::vec4 ndc(x ? 1.0f : -1.0f, y ? 1.0f : -1.0f, z ? 1.0f : 0.0f, 1.0f);
                    const glm::vec4 w = inv_cam * ndc;
                    corners[ci++] = glm::vec3(w) / w.w;
                }
        // Split the near/far corner pairs (indices z=0 far, z=1 near in reverse-Z above) so this
        // cascade covers [last_split, split] of the camera's linear depth. March along the frustum
        // edge FROM the near corner TOWARD the far corner by (view_depth - near_clip)/range: at
        // last_split we get this cascade's near edge, at split its far edge. (The previous code based
        // the lerp on the FAR corner with an inverted parameter, so last_split=near_clip returned the
        // far corner — planting every cascade kilometres out at the camera far plane, ~2 m/texel, so
        // the scene got ~15 texels of shadow and nothing resolved.)
        // The frustum edge spans the CAMERA's full depth (near .. camera far plane), so the lerp
        // parameter that maps a view-space depth onto that edge must divide by the camera's far
        // distance, NOT shadow_depth_range. (`range` above is only for the split SCHEME over
        // [near, shadow_depth_range]; using it here left cascades ~camera_far/shadow_range x too big.)
        const float cam_range = camera_.far_plane() - near_clip;
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec3 far_c = corners[i * 2 + 0];   // z=0 -> camera far plane
            const glm::vec3 near_c = corners[i * 2 + 1];  // z=1 -> camera near plane
            const glm::vec3 edge = far_c - near_c;        // near -> far along this frustum edge
            corners[i * 2 + 0] = near_c + edge * ((split - near_clip) / cam_range);       // cascade far edge
            corners[i * 2 + 1] = near_c + edge * ((last_split - near_clip) / cam_range);  // cascade near edge
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners) center += corner;
        center /= 8.0f;

        float radius = 0.0f;
        for (const glm::vec3& corner : corners) radius = std::max(radius, glm::length(corner - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantize the radius so it doesn't jitter

        const float texel = (2.0f * radius) / static_cast<float>(settings_.shadow_resolution);
        cascade_world_texel_[c] = texel;

        // Depth range: bracket the SCENE AABB along the light axis (a BOUNDED extent), NOT the cascade
        // radius. Pulling the eye back by `radius` blew the far cascades' depth range (radius >> the
        // 30 m scene, so cascade 2 got a ~1.5 km-deep box) — depth precision collapsed and distant /
        // grazing floor lost its shadows on rotation. Projecting the scene AABB onto L gives the true
        // caster..receiver span; the eye sits just behind the nearest-to-sun caster, far reaches the
        // farthest receiver. XY still uses `radius`; only near/far come from the scene.
        float min_proj = std::numeric_limits<float>::max();
        float max_proj = std::numeric_limits<float>::lowest();
        for (int ci = 0; ci < 8; ++ci)
        {
            const glm::vec3 corner((ci & 1) ? scene_aabb_max_.x : scene_aabb_min_.x,
                                   (ci & 2) ? scene_aabb_max_.y : scene_aabb_min_.y,
                                   (ci & 4) ? scene_aabb_max_.z : scene_aabb_min_.z);
            const float d = glm::dot(corner - center, L);   // signed distance along L (toward sun = +)
            min_proj = std::min(min_proj, d);
            max_proj = std::max(max_proj, d);
        }
        const float margin = 1.0f;
        const float eye_back = max_proj + margin;                   // eye just behind the sun-most caster
        const float far_d = (max_proj - min_proj) + 2.0f * margin;  // reach the farthest receiver
        const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 eye = center + L * eye_back;
        glm::mat4 view = glm::lookAt(eye, center, up);

        // Snap the center to whole texels in light space (stabilization).
        glm::vec3 center_ls = glm::vec3(view * glm::vec4(center, 1.0f));
        center_ls.x = std::floor(center_ls.x / texel) * texel;
        center_ls.y = std::floor(center_ls.y / texel) * texel;
        const glm::vec3 snapped_center = glm::vec3(glm::inverse(view) * glm::vec4(center_ls, 1.0f));
        const glm::vec3 snapped_eye = snapped_center + L * eye_back;
        view = glm::lookAt(snapped_eye, snapped_center, up);

        // Ortho box: [-radius, radius] in x/y; near=0 (at the eye), far = scene depth span along L.
        glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, far_d);
        proj[1][1] *= -1.0f;  // Vulkan Y-flip
        glm::mat4 reverse_z(1.0f);
        reverse_z[2][2] = -1.0f;
        reverse_z[3][2] = 1.0f;
        proj = reverse_z * proj;

        cascade_view_proj_[c] = proj * view;
        // Brief 04 M4: conservative world-space bounding sphere of this cascade for the draw-level
        // shadow cull. XY is bounded by `radius`; the ortho depth now spans the scene AABB along L
        // (far_d), so inflate the cull radius to cover the XY disc + full depth slab. Conservative
        // (never under-covers) -> off-cascade casters are safely kept.
        cascade_center_[c] = center;
        cascade_cull_radius_[c] = radius + far_d;
        last_split = split;
    }
}

// Time-of-day 0..1 -> a sun direction arc (east low -> zenith -> west low) and the sky/sun palette.
namespace
{
struct SunState { glm::vec3 dir; glm::vec3 sun_color; float sun_intensity; glm::vec3 sky_zenith; glm::vec3 sky_ground; };
SunState sun_for_time(float t)
{
    // Elevation follows a half-sine over the day; azimuth sweeps east->west. Below the horizon is
    // clamped a little above so the scene never goes pitch black (stylized dusk, not night).
    const float angle = t * glm::pi<float>();                  // 0..pi across the arc
    const float elevation = std::sin(angle) * 0.85f + 0.06f;   // never fully below horizon
    const float azimuth = (t - 0.5f) * 2.4f;                   // east -> west sweep
    const float horiz = std::sqrt(std::max(1.0f - elevation * elevation, 0.0f));
    SunState s;
    s.dir = glm::normalize(glm::vec3(std::sin(azimuth) * horiz, elevation, std::cos(azimuth) * horiz));
    // Warm at the horizon (sunrise/sunset), neutral-bright at noon.
    const float noon = glm::clamp(elevation, 0.0f, 1.0f);
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

void GeometryPass::build_light_stress_scene(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
{
    // Hundreds of colored point + spot lights orbiting over the model — the brief's light stress test
    // bed. Deterministic pseudo-random placement so runs are comparable. Radii/intensities sized to
    // the scene so froxel binning is meaningfully exercised without washing everything out.
    constexpr uint32_t kStressLights = 384;
    const glm::vec3 extent = aabb_max - aabb_min;
    const glm::vec3 center = (aabb_min + aabb_max) * 0.5f;
    const float scene_scale = glm::length(extent);
    const float light_range = scene_scale * 0.06f;

    uint32_t seed = 0x1234567u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / static_cast<float>(0xFFFFFF); };

    lights_.clear();
    light_anim_.clear();
    lights_.reserve(kStressLights);
    light_anim_.reserve(kStressLights);
    for (uint32_t i = 0; i < kStressLights; ++i)
    {
        const bool spot = (i % 4) == 0;
        // A warm/saturated palette so overlapping lights read distinctly.
        const glm::vec3 color = glm::vec3(0.4f + 0.6f * rnd(), 0.4f + 0.6f * rnd(), 0.4f + 0.6f * rnd());
        LightAnim a;
        a.center = center + glm::vec3((rnd() - 0.5f) * extent.x, aabb_min.y + extent.y * (0.15f + 0.5f * rnd()),
                                      (rnd() - 0.5f) * extent.z);
        a.radius = extent.x * (0.05f + 0.25f * rnd());
        a.speed = (0.3f + 1.2f * rnd()) * (rnd() > 0.5f ? 1.0f : -1.0f);
        a.phase = rnd() * 6.2831853f;
        a.height = extent.y * 0.15f * rnd();
        light_anim_.push_back(a);

        GpuLight L{};
        L.position_radius = glm::vec4(a.center, light_range);
        // Brief 07 M4 units: luminous intensity in kilocandela (illuminance = I/d^2 in klx).
        // Stress lights are deliberately floodlight-class so they still read against daylight.
        L.color_intensity = glm::vec4(color, spot ? 300.0f : 150.0f);
        const glm::vec3 dir = glm::normalize(glm::vec3(rnd() - 0.5f, -1.0f, rnd() - 0.5f));
        L.direction_type = glm::vec4(dir, spot ? 1.0f : 0.0f);
        L.cone = glm::vec4(std::cos(glm::radians(18.0f)), std::cos(glm::radians(30.0f)), 0.0f, 0.0f);
        lights_.push_back(L);
    }
    STRING_LOG_INFO("[light] stress scene: {} lights ({} spot), range {:.2f}",
                    lights_.size(), kStressLights / 4, light_range);
}

void GeometryPass::animate_lights(float delta_time)
{
    static float t = 0.0f;
    t += delta_time;
    for (std::size_t i = 0; i < lights_.size(); ++i)
    {
        const LightAnim& a = light_anim_[i];
        const float ang = a.phase + t * a.speed;
        lights_[i].position_radius.x = a.center.x + std::cos(ang) * a.radius;
        lights_[i].position_radius.z = a.center.z + std::sin(ang) * a.radius;
        lights_[i].position_radius.y = a.center.y + std::sin(ang * 1.7f) * a.height;
    }
}

void GeometryPass::ensure_froxel_capacity()
{
    if (screen_size.width == 0 || screen_size.height == 0)
    {
        return;
    }
    froxel_tiles_x_ = (screen_size.width + kFroxelTileSize - 1) / kFroxelTileSize;
    froxel_tiles_y_ = (screen_size.height + kFroxelTileSize - 1) / kFroxelTileSize;
    froxel_count_ = froxel_tiles_x_ * froxel_tiles_y_ * kFroxelDepthSlices;
    if (froxel_count_ <= froxel_capacity_ && froxel_buffers_[0] != 0)
    {
        return;   // fits the existing allocation
    }
    // Grow (or first allocation). The renderer waits the device idle on resize, so recreating these
    // device-addressed buffers here is safe. Stride = [count, idx...] = 1 + max per froxel.
    const VkDeviceSize stride = (1 + kMaxLightsPerFroxel) * sizeof(uint32_t);
    const VkDeviceSize size = stride * froxel_count_;
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        if (froxel_buffers_[f] != 0) allocator_.destroy_resource(froxel_buffers_[f]);
        froxel_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
    }
    froxel_capacity_ = froxel_count_;
    STRING_LOG_INFO("[froxel] grid {}x{}x{} = {} froxels ({} MB/frame)", froxel_tiles_x_,
                    froxel_tiles_y_, kFroxelDepthSlices, froxel_count_, size / (1024 * 1024));
}

GeometryPass::~GeometryPass()
{
    auto destroy_program = [this](string::gpu::shader_program* prog) {
        if (!prog) return;
        const string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(sky_program_);
    destroy_program(froxel_program_);
    destroy_program(env_capture_program_);
    destroy_program(env_mip_program_);
    destroy_program(env_prefilter_program_);
    destroy_program(sh_project_program_);
    destroy_program(dfg_program_);
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
            if (hz.image != 0)
            {
                descriptor_table_.unbind(hz.image, string::gpu::descriptor_type::TEXTURE);
                allocator_.destroy_resource(hz.image);
            }
            if (hz.depth != 0)
            {
                descriptor_table_.unbind(hz.depth, string::gpu::descriptor_type::TEXTURE);
                allocator_.destroy_resource(hz.depth);
            }
        }
        if (hiz_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_.get_device(), hiz_sampler_, nullptr);
        // Brief 09 GTAO targets.
        if (gtao_raw_storage_slot_ != UINT32_MAX)
            descriptor_table_.unbind_storage_view(gtao_raw_storage_slot_);
        for (uint32_t s : gtao_final_storage_slots_) descriptor_table_.unbind_storage_view(s);
        if (gtao_raw_ != 0)
        {
            descriptor_table_.unbind(gtao_raw_, string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(gtao_raw_);
        }
        for (string::gpu::resource_id id : gtao_final_)
            if (id != 0)
            {
                descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
                allocator_.destroy_resource(id);
            }
        for (const string::gpu::resource_id b : stats_buffers_) allocator_.destroy_resource(b);
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
        for (const string::gpu::resource_id b : scene_buffers_) allocator_.destroy_resource(b);
        for (const string::gpu::resource_id b : light_buffers_) allocator_.destroy_resource(b);
        for (const string::gpu::resource_id b : froxel_buffers_)
            if (b != 0) allocator_.destroy_resource(b);
    }

    // Brief 07 IBL resources.
    if (env_capture_ != 0)
    {
        const auto drop_cube = [&](string::gpu::resource_id id, std::vector<VkImageView>& views,
                                   std::vector<uint32_t>& slots) {
            for (uint32_t s : slots) descriptor_table_.unbind_storage_view(s);
            for (VkImageView v : views) vkDestroyImageView(device_.get_device(), v, nullptr);
            descriptor_table_.unbind(id, string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(id);
        };
        drop_cube(env_capture_, env_capture_mip_views_, env_capture_mip_slots_);
        drop_cube(env_prefiltered_, env_prefiltered_mip_views_, env_prefiltered_mip_slots_);
        descriptor_table_.unbind_storage_view(dfg_storage_slot_);
        descriptor_table_.unbind(dfg_lut_, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(dfg_lut_);
        allocator_.destroy_resource(sh_buffer_);
        vkDestroySampler(device_.get_device(), env_sampler_, nullptr);
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
    if (env_capture_ != 0)
    {
        constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg)
        const float align = glm::dot(glm::normalize(sun_dir_), ibl_captured_sun_dir_);
        if (!ibl_primed_ || cv_ibl_every_frame().get() || furnace_ != ibl_captured_furnace_
            || align < kSunDeltaCos)
            ibl_update_pending_ = true;

        // M1 numeric gate (dbg.ibl_verify): after the chain has settled, read the DFG LUT + SH
        // coefficients back and check them against CPU references. Stalls the device; debug only.
        if (cv_ibl_verify().get() && stream_frame_ == 40)
            run_ibl_verification();
    }

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
        if (ibl_primed_ && (!probe_primed_ || cv_ibl_every_frame().get() || align < kSunDeltaCos))
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
    if (draw_count_ > 0 && current_frame < scene_buffers_.size())
    {
        ensure_froxel_capacity();

        // Upload this frame's animated lights (or none, if the stress set is toggled off).
        const uint32_t light_count = lights_enabled_ ? static_cast<uint32_t>(lights_.size()) : 0u;
        if (light_count > 0)
        {
            std::memcpy(light_mapped_[current_frame], lights_.data(), sizeof(GpuLight) * light_count);
        }

        // Fill SceneData for this frame. The lit shader reads sun/ambient/CSM/froxel state from here.
        SceneData scene{};
        scene.camera_pos = camera_.position();
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
        scene.cascade_count = settings_.cascade_count;
        scene.shadow_texel = 1.0f / static_cast<float>(settings_.shadow_resolution);
        scene.shadow_bias = cv_shadow_bias().get();
        scene.shadow_normal_offset_scale = cv_shadow_normal_offset().get();
        scene.cascade_blend = settings_.cascade_blend;
        scene.view = camera_.view();
        scene.froxel_dims = glm::uvec4(froxel_tiles_x_, froxel_tiles_y_, kFroxelDepthSlices, kFroxelTileSize);
        const float near_p = camera_.near_plane();
        const float far_p = std::min(settings_.shadow_depth_range, camera_.far_plane());
        scene.froxel_planes = glm::vec4(near_p, far_p, 1.0f / std::log(far_p / near_p),
                                        static_cast<float>(light_count));
        scene.lights = light_count > 0 ? allocator_.get_buffer(light_buffers_[current_frame]).device_address : 0;
        scene.froxels = froxel_buffers_[current_frame] != 0
                      ? allocator_.get_buffer(froxel_buffers_[current_frame]).device_address : 0;
        scene.max_lights_per_froxel = kMaxLightsPerFroxel;
        scene.debug_flags = (froxel_heatmap_ ? 1u : 0u) | (furnace_ ? 2u : 0u)
                          | (cv_gtao_spec_occ().get() ? 0u : 4u)    // bit2: disable bent-normal spec-occ
                          | ((static_cast<uint32_t>(std::max(cv_light_debug().get(), 0)) & 0xFu) << 4);  // bits4-7: lighting isolate
        // Brief 07: the sky-IBL products (single-buffered; the update chain is ordered against
        // in-flight readers inside record_ibl_update / by the declared SH usages).
        scene.sh = sh_buffer_ != 0 ? allocator_.get_buffer(sh_buffer_).device_address : 0;
        scene.env_slot = env_prefiltered_slot_;
        scene.env_mips = kEnvPrefilterMips;
        scene.dfg_slot = dfg_sample_slot_;

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

        std::memcpy(scene_mapped_[current_frame], &scene, sizeof(SceneData));
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
        if (current_frame < stats_buffers_.size())
        {
            const void* mapped = allocator_.get_buffer(stats_buffers_[current_frame]).allocation_info.pMappedData;
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
                            stream_frame_, ibl_update_count_, sun_animate_ ? "animating" : "");
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
    // Brief 04e M4: the froxel list is written by the pass's ASYNC compute chain and read by the
    // main-queue fragment stage — declared in async_usages so the renderer derives the timeline
    // edge + ownership transfer (async lane) or the plain barriers (inline fallback).
    async_usages.clear();
    if (current_frame < froxel_buffers_.size() && froxel_buffers_[current_frame] != 0)
    {
        async_usages.push_back({ froxel_buffers_[current_frame], String::Access::StorageWrite,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
        async_usages.push_back({ froxel_buffers_[current_frame], String::Access::StorageRead,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT });
    }
    // Per-frame stats: reset + histogram in compute, then atomic tallies from the task/mesh/
    // fragment stages of the draws. Declaring the draw-stage RMW gives it a derived
    // compute->draw-stages WAW barrier (the old broad hand barrier never covered the TASK/MESH
    // stats atomics — a latent narrowness this closes).
    if (current_frame < stats_buffers_.size() && stats_buffers_[current_frame] != 0)
    {
        usages.push_back({ stats_buffers_[current_frame], String::Access::StorageWrite,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
        usages.push_back({ stats_buffers_[current_frame], String::Access::StorageWrite,
                           VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT
                               | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT
                               | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT });
    }
    // Brief 07: the SH coefficient buffer — written by the IBL compute chain on update frames,
    // read by every lit fragment. The graph derives the compute->fragment barrier (and the
    // cross-frame ordering) from these declarations; the cubemap/DFG images use intra-pass
    // barriers (documented local class, like the HiZ mip chain).
    if (sh_buffer_ != 0)
    {
        if (ibl_update_pending_)
            usages.push_back({ sh_buffer_, String::Access::StorageWrite,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
        usages.push_back({ sh_buffer_, String::Access::StorageRead,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT });
    }
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

    // The HiZ (re)build binds new per-mip storage views into the bindless set — descriptor
    // UPDATES, and the set has no UPDATE_AFTER_BIND flag, so they must land BEFORE anything in
    // this command buffer binds the set (the IBL chain below is the first binder).
    if (meshlet_program_ && draw_info_mapped_) ensure_hiz(current_frame);

    // --- Brief 09: GTAO chain (prev-frame depth -> this slot's AO+bent target) -----------------
    // Runs first so this frame's fragments (phase 1 onward) read a finished AO texture. The
    // go/no-go decision was made in update() (SceneData.gtao_slot must agree with what actually
    // records). ensure_gtao() ran in update() — its descriptor updates precede every set bind.
    if (gtao_runs_this_frame_)
        record_gtao(command_buffer, current_frame);

    // --- Brief 07: sky-IBL update chain (amortized; see the trigger in update()) ---------------
    // Recorded before everything else so the frame's draws read this frame's environment. The
    // whole chain updates in ONE frame (capture -> mips -> SH + prefilter), so the ambient is
    // always self-consistent — amortization can never pop mid-update.
    if ((ibl_update_pending_ || !dfg_baked_) && env_capture_program_ != nullptr && env_capture_ != 0)
    {
        STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "ibl-update")
        record_ibl_update(command_buffer);
        ibl_update_pending_ = false;
    }

    // --- Brief 09b: probe volume — static capture (once) + dynamic relight (amortized) ----------
    // Ordered after the IBL chain so relight's sky-SH source is this frame's SH. Capture is a
    // one-time init (M0) / raster (M1); relight re-runs whenever the sun moved (see update()).
    if (probe_volume_.valid && cv_gi_enabled().get() && probe_clear_program_ != nullptr)
    {
        if (!probe_captured_)
        {
            STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "gi-capture")
            record_probe_capture(command_buffer);
        }
        // M2: relight starts only after the capture COMPLETES — partially-captured probes would
        // relight from the cleared (all-sky-miss) capture and read as outdoor probes until their
        // capture landed. It also guarantees the CSM shadow maps this slot samples have been
        // rendered at least once. record_probe_relight manages the cursor/pending state itself
        // (pending stays true until the armed converge passes are exhausted).
        if (probe_captured_ && probe_relight_pending_ && ibl_primed_)
        {
            STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "gi-relight")
            record_probe_relight(command_buffer, current_frame);
        }
    }

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
                .stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address,
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
                record_draw_cull(command_buffer, current_frame);
                if (do_shadow)
                    for (uint32_t c = 0; c < settings_.cascade_count && c < kMaxCascades; ++c)
                        record_draw_cull(command_buffer, current_frame, /*cascade*/ int(c));
            }
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "expand")
                const VkDeviceAddress cam_lod =
                    draw_lod_address(current_frame);
                record_expand(command_buffer, wl_opaque_[current_frame], cam_lod);
                record_expand(command_buffer, wl_twosided_[current_frame], cam_lod);
                if (do_shadow)
                    for (uint32_t c = 0; c < settings_.cascade_count && c < kMaxCascades; ++c)
                        record_expand(command_buffer, wl_shadow_[current_frame][c], cam_lod);
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
    // --- Cascaded shadow maps: render scene depth from the sun into each cascade's image, then
    // transition each to SHADER_READ for the lit pass. Draws the resident set via the meshlet shadow
    // path (off-screen casters included — cascades read the resident-only work list, NOT camera-culled).
    if (!shadow_images_.empty() && meshlet_shadow_program_ && draw_info_mapped_)
    {
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

    // Procedural sky first (fullscreen, no depth) so opaque geometry overwrites it where it exists;
    // the shared sky colours also drive the lit shader's ambient below.
    {
        STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "sky")
        const glm::mat4 vp = camera_.view_proj();
        const SkyPush sky_push{
            .inv_view_proj = glm::inverse(vp),
            .camera_pos = camera_.position(),
            .furnace = furnace_ ? 1.0f : 0.0f,
            .sun_dir = sun_dir_,
            .sky_zenith = sky_zenith_,
            .sky_ground = sky_ground_,   // albedo; radiance derived in-shader
            .sun_color = sun_color_,
            .sun_intensity = sun_intensity_,
        };
        const string::gpu::pipeline& sky_p = sky_program_->current();
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_p.pipeline);
        vkCmdPushConstants(command_buffer, sky_p.pipeline_layout,
                           sky_p.push_constants.stageFlags, 0, sizeof(SkyPush), &sky_push);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
    }

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
            record_opaque_phase(command_buffer, current_frame, /*phase*/ 1u);
        }
        else
        {
            // Single-pass (HiZ off / warmup): frustum+cone+HiZ in one group, then transparency — the
            // legacy phase-0 flow. breaks_scene_group() is false so this all lands in one render group.
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
                record_opaque_phase(command_buffer, current_frame, /*phase*/ 0u);
            }
            // Brief 09b: probe-debug spheres after opaque, before transparency.
            record_probe_debug(command_buffer);
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "transparency")
                const uint32_t transp_count = build_transparency_list(current_frame);
                record_transparency(command_buffer, current_frame, transp_count);
            }
        }
    }
}

// Brief 04d: draw the opaque one-sided + two-sided camera lists at `phase` (0 legacy / 1 bit-set /
// 2 bit-clear+HiZ+update). Assumes an MSAA scene render pass is already open (record() for phase 1/0,
// record_after_between() for phase 2). Shared by both phases — the task shader does the partitioning.
void GeometryPass::record_opaque_phase(VkCommandBuffer command_buffer, uint16_t current_frame, uint32_t phase)
{
    const string::gpu::pipeline& mp = meshlet_program_->current();
    VkDescriptorSet mset = descriptor_table_.get_set();
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mp.pipeline_layout, 0, 1, &mset, 0, nullptr);
    record_meshlet_draws(command_buffer, mp, current_frame, wl_opaque_[current_frame], phase);
    if (meshlet_twosided_program_)
    {
        const string::gpu::pipeline& tp = meshlet_twosided_program_->current();
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tp.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                tp.pipeline_layout, 0, 1, &mset, 0, nullptr);
        record_meshlet_draws(command_buffer, tp, current_frame, wl_twosided_[current_frame], phase);
    }
}

// Brief 04d: the depth-resolve target the renderer MIN-resolves the phase-1 MSAA depth into (the
// pyramid's single-sample mip-0 source). Valid only when the two-phase path is active this frame.
string::gpu::resource_id GeometryPass::depth_resolve_target(uint16_t current_frame) const
{
    if (!two_phase_active_ || current_frame >= hiz_.size()) return 0;
    return hiz_[current_frame].depth;
}

// Brief 04d: build the HiZ pyramid from the MIN-resolved phase-1 depth (hz.depth), OUTSIDE rendering.
// The renderer already resolved msaa_depth_ -> hz.depth (reverse-Z MIN = farthest) at phase-1's
// EndRendering and left it in DEPTH_ATTACHMENT layout. Reuses the exact mip-0 uniform-stretch +
// 2x2 min chain the old prepass path used; only the depth SOURCE changed (resolve, not a re-draw).
void GeometryPass::record_between(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (!two_phase_active_ || !hiz_program_ || current_frame >= hiz_.size()) return;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    const HizPyramid& hz = hiz_[current_frame];
    const string::gpu::allocated_image& depth = allocator_.get_image(hz.depth);

    // Bitfield hazard: phase-1's task shader READ the bits; phase-2's task shader will READ-MODIFY-
    // WRITE them. Serialize task-stage bitfield access across the phase boundary.
    {
        const VkMemoryBarrier2 bf = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        const VkDependencyInfo bdep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &bf };
        vkCmdPipelineBarrier2(command_buffer, &bdep);
    }

    // Resolved depth (DEPTH_ATTACHMENT, resolve dest) -> SHADER_READ for the mip-0 reduce.
    // Vulkan resolve operations (incl. the reverse-Z MIN depth resolve at phase-1's EndRendering)
    // execute in COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE, NOT the depth stages — so the
    // src scope that waits on the resolve write must name that stage/access (sync-validation clean).
    vku::transition_image(command_buffer, {
        .image = depth.image, .old_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
    });
    const string::gpu::allocated_image& pyr = allocator_.get_image(hz.image);
    vku::transition_image(command_buffer, {
        .image = pyr.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .level_count = hz.mips,
    });
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
    // Whole pyramid -> SHADER_READ for the phase-2 task shader's SampleLevel.
    vku::transition_image(command_buffer, {
        .image = pyr.image, .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .level_count = hz.mips,
    });
    // Return hz.depth to DEPTH_ATTACHMENT so the phase-2 group can keep depth-testing/writing the MSAA
    // depth (the resolve target is separate; this keeps its tracker state consistent for next frame's
    // resolve). Not strictly required for correctness this frame but keeps layouts predictable.
    vku::transition_image(command_buffer, {
        .image = depth.image, .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
    });

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
void GeometryPass::record_after_between(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (!meshlet_program_ || !draw_info_mapped_) return;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    record_opaque_phase(command_buffer, current_frame, /*phase*/ 2u);
    // Brief 09b: probe-debug spheres after opaque, before transparency (opaque, depth-tested).
    record_probe_debug(command_buffer);
    const uint32_t transp_count = build_transparency_list(current_frame);
    record_transparency(command_buffer, current_frame, transp_count);
}

}  // namespace sandbox
