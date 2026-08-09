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
#include <unordered_set>
#include <utility>
#include <vector>

#include <string/platform/user_dirs.hpp>
#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>
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

// libktx: cooked .ktx2 textures are already BC7 (see string-asset-tools texture_cook) and stream in without
// any CPU pixel decode or transcode — the fast path that avoids the stb_image load-time floor.
// ktxvulkan.h (VkFormat query) requires the Vulkan headers above it and pulls in ktx.h itself.
#include <ktxvulkan.h>

namespace string::render
{
using namespace string;

namespace
{

// The renderer's debug keys, interned once. Queries use these constants so a typo is a compile
// error; binding goes through the string overload deliberately (brief 17: that is what registers the
// name the rebinding UI reads back).
namespace debug_actions
{
inline constexpr ::string::ActionId freeze_culling   = ::string::action_id("freeze_culling");
inline constexpr ::string::ActionId toggle_culling   = ::string::action_id("toggle_culling");
inline constexpr ::string::ActionId toggle_hiz       = ::string::action_id("toggle_hiz");
inline constexpr ::string::ActionId cycle_debug_view = ::string::action_id("cycle_debug_view");
inline constexpr ::string::ActionId toggle_crowd     = ::string::action_id("toggle_crowd");
inline constexpr ::string::ActionId toggle_lod       = ::string::action_id("toggle_lod");
inline constexpr ::string::ActionId sun_animate      = ::string::action_id("sun_animate");
inline constexpr ::string::ActionId time_back        = ::string::action_id("time_back");
inline constexpr ::string::ActionId time_fwd         = ::string::action_id("time_fwd");
inline constexpr ::string::ActionId toggle_lights    = ::string::action_id("toggle_lights");
inline constexpr ::string::ActionId toggle_heatmap   = ::string::action_id("toggle_heatmap");
}  // namespace debug_actions

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

// A 1x1 opaque white stand-in. Multiplying by white is the identity for every slot that takes a
// colour map, so a texture we could not decode costs that surface its detail and nothing else.
DecodedTexture white_fallback(bool srgb)
{
    auto* pixels = static_cast<stbi_uc*>(STBI_MALLOC(4));
    pixels[0] = pixels[1] = pixels[2] = pixels[3] = 0xFF;
    DecodedTexture decoded;
    decoded.pixels.reset(pixels);
    decoded.width = 1;
    decoded.height = 1;
    decoded.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return decoded;
}

// Decode one texture source (file or embedded bytes) to RGBA8. Pure CPU work — safe to run on a job
// thread. NEVER throws: a texture is content, and bad content must not be able to kill the engine.
// This used to throw, and because a source with neither a file nor bytes decodes to nothing, a .glb
// whose embedded images had been dropped at bake reached here and took the process down.
DecodedTexture decode_texture(const GltfTexture& source)
{
    if (source.file.empty() && source.encoded.empty())
    {
        STRING_LOG_WARN("[load] texture has no source (embedded image lost at bake?); using white");
        return white_fallback(source.srgb);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = source.file.empty()
        ? stbi_load_from_memory(source.encoded.data(), static_cast<int>(source.encoded.size()),
                                &width, &height, &channels, STBI_rgb_alpha)
        : stbi_load(source.file.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0)
    {
        if (pixels) stbi_image_free(pixels);
        STRING_LOG_WARN("[load] failed to decode texture {}: {}; using white",
                        source.file.empty() ? std::string("<embedded>") : source.file.string(),
                        stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return white_fallback(source.srgb);
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
void upload_decoded(::string::gpu::resource_allocator& allocator,
                    ::string::gpu::descriptor_table& descriptor_table, TransferBatch& transfer,
                    const DecodedTexture& decoded,
                    ::string::gpu::resource_id& out_image, uint32_t& out_slot)
{
    // Full mip chain: floor(log2(max dimension)) + 1 levels. TRANSFER_SRC is needed too because
    // mip generation blits from each level down to the next.
    const uint32_t max_dim = static_cast<uint32_t>(std::max(decoded.width, decoded.height));
    const uint32_t mip_levels = static_cast<uint32_t>(std::floor(std::log2(max_dim))) + 1;

    out_image = allocator.create_resource(::string::gpu::image_info{
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

    descriptor_table.bind(out_image, ::string::gpu::descriptor_type::TEXTURE);
    out_slot = descriptor_table.get_binding_slot(out_image, ::string::gpu::descriptor_type::TEXTURE);
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
LoadedScene build_lookdev_scene()
{
    std::vector<string::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    std::vector<GltfMaterial> materials;

    // Unit-sphere template (UV sphere; normal = position, tangent along +phi).
    constexpr int kStacks = 24, kSlices = 48;
    std::vector<string::Vertex> sphere_verts;
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
            sphere_verts.push_back(string::Vertex{ n, glm::vec3(1.0f),
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
        for (string::Vertex v : sphere_verts)
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
            vertices.push_back(string::Vertex{ corners[i], glm::vec3(1.0f), uvs[i], n, tan });
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
    CookedScene cs = ::string::asset::tools::bake_scene(vertices, indices, draws, ::string::asset::tools::BakeParams{});

    LoadedScene out;
    out.vertices = std::move(cs.vertices);
    out.meshlets = std::move(cs.meshlets);
    out.meshlet_vertices = std::move(cs.meshlet_vertices);
    out.meshlet_triangles = std::move(cs.meshlet_triangles);
    out.total_meshlets = cs.total_meshlets;
    out.materials = std::move(materials);
    for (const CookedDraw& cd : cs.draws)
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

geometry_pass::geometry_pass(engine_context& context, VkSampleCountFlagBits samples,
                             std::vector<std::filesystem::path> model_paths,
                           std::shared_ptr<MeshOverlayStats> overlay_stats, bool lookdev)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, residency_(kTextureBudget, kTextureStreamPerFrame)
, gpu_profiler_ctx_(context.gpu_profiler_ctx)
{
    scene_samples_ = samples;
    // These live on the GeometryScene base, and a base's members cannot sit in the derived init
    // list, so seed them first thing in the body (nothing above uses them).
    frames_in_flight_ = context.frames_in_flight;
    overlay_stats_ = std::move(overlay_stats);
    // The whole geometry pass is CVar-toggleable (r.pass.geometry); off -> composite reads the
    // cleared color target. Touch it NOW so it self-registers at construction, before the env
    // override is applied, rather than lazily at frame 1. The toggle is declared in declare().
    (void)cv_pass_geometry();
    // Brief 21 D4: the work lists are graph transients the app declares; nothing is reserved here.
    // Brief 16 M1: the resource-virtualization hub, for the registry-owned SceneData ring (below)
    // and, later, the light/stats rings (M3).
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
    LoadedScene loaded = lookdev_
        ? build_lookdev_scene()
        : load_cooked_scenes(context.resources_path, model_paths, chunk_budget);

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
        std::vector<string::Vertex> qv;
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
                qv.push_back(string::Vertex{ c, col, { 0.0f, 0.0f }, nrm, 0u });
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
        CookedScene qs = ::string::asset::tools::bake_scene(qv, qi, qd, ::string::asset::tools::BakeParams{});
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
        for (const CookedDraw& cd : qs.draws)
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
    const VkDeviceSize vertex_size = VkDeviceSize(sizeof(string::Vertex)) * vertex_capacity;
    // Brief 04 M3/04b: no GPU index heap AND no CPU index buffer — the task/mesh shaders pull vertices
    // via the meshlet-vertex remap heap (baked at cook); the streamer uploads per-draw vertex windows.

    // Model AABB, computed now (before the geometry arrays are moved into the streamer below) for
    // framing the camera.
    // WORLD space, from the per-draw bounds — not the raw vertex positions, which are MODEL space.
    // A glTF places its meshes with node transforms, so an asset authored around the origin and then
    // positioned somewhere else has vertices that say one thing and a location that says another.
    // Framing the model-space box aims the camera at empty space, which reads as "I loaded my asset
    // and there is nothing there". The cook already computed each draw's world AABB (it transforms
    // the 8 corners); union those.
    glm::vec3 aabb_min(std::numeric_limits<float>::max());
    glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
    for (const auto& d : geometry.draws)
    {
        aabb_min = glm::min(aabb_min, d.aabb_min);
        aabb_max = glm::max(aabb_max, d.aabb_max);
    }
    // Fall back to model space only if no draw carried bounds, and to a unit box if there is no
    // geometry at all — a degenerate box would make the framing maths produce NaNs.
    if (geometry.draws.empty())
    {
        for (const auto& vertex : geometry.vertices)
        {
            aabb_min = glm::min(aabb_min, vertex.pos);
            aabb_max = glm::max(aabb_max, vertex.pos);
        }
    }
    if (!(aabb_min.x <= aabb_max.x && aabb_min.y <= aabb_max.y && aabb_min.z <= aabb_max.z))
    {
        aabb_min = glm::vec3(-1.0f);
        aabb_max = glm::vec3(1.0f);
    }

    // Shared geometry buffers allocated whole up front, but NOT uploaded here: the geometry
    // streamer uploads each draw's vertex/index sub-range on demand (when the draw enters view).
    // Vertices are pulled by device address in the vertex shader, so the buffer needs
    // SHADER_DEVICE_ADDRESS (which populates allocated_buffer.device_address) rather than VERTEX_BUFFER.
    vertex_buffer_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
               | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // 1x1 white fallback for draws without a base-color texture (the factor still tints it).
    const std::array<uint8_t, 4> white_pixel = { 255, 255, 255, 255 };
    white_image_ = allocator_.create_resource(::string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_image(white_pixel.data(), white_pixel.size(), white_image_);
    descriptor_table_.bind(white_image_, ::string::gpu::descriptor_type::TEXTURE);
    white_slot_ = descriptor_table_.get_binding_slot(white_image_, ::string::gpu::descriptor_type::TEXTURE);

    // 1x1 flat-normal fallback (tangent-space +Z): draws with no normal map sample this and get the
    // geometric normal back, so the fragment shader never branches on "has a normal map".
    const std::array<uint8_t, 4> flat_normal_pixel = { 128, 128, 255, 255 };
    flat_normal_image_ = allocator_.create_resource(::string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,   // linear, not sRGB — it's data, not colour
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_image(flat_normal_pixel.data(), flat_normal_pixel.size(), flat_normal_image_);
    descriptor_table_.bind(flat_normal_image_, ::string::gpu::descriptor_type::TEXTURE);
    flat_normal_slot_ = descriptor_table_.get_binding_slot(flat_normal_image_, ::string::gpu::descriptor_type::TEXTURE);

    // Texture sources come from the cooked scenes (path + srgb). Cooked .ktx2 siblings register with
    // the streamer; the rest decode via stb on a worker pool. (Textures are unchanged this brief; the
    // cooked format just carries the paths, so the decode/upload path below is the same as before.)
    ::string::core::job_system decode_pool;
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
        allocator_, descriptor_table_, context.transfer);
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
            const VkDeviceSize geometry_budget = VkDeviceSize(vertex_capacity) * sizeof(string::Vertex);
            geometry_residency_ = std::make_unique<::string::gpu::residency_manager>(
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

    // The bounds the camera is about to be framed on. Printed because "I loaded my asset and see
    // nothing" and "my asset is somewhere unexpected" are the same symptom, and this is the number
    // that separates them — it is also what you need to aim STRING_CAM at a specific part of a model.
    STRING_LOG_INFO("[scene] world bounds ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})",
                    aabb_min.x, aabb_min.y, aabb_min.z, aabb_max.x, aabb_max.y, aabb_max.z);

    // Frame the whole model with the engine camera using the AABB computed above, then let it
    // position itself to fit. Bind the conventional fly controls (WASD + Space/Ctrl + Shift) onto
    // the shared InputMap so update() can read them by action name.
    string::Camera::bind_default_controls(input_map_);
    input_map_.bind_button("freeze_culling", string::KeyCode::F);
    input_map_.bind_button("toggle_culling", string::KeyCode::C);
    input_map_.bind_button("toggle_hiz", string::KeyCode::O);
    input_map_.bind_button("cycle_debug_view", string::KeyCode::V);
    input_map_.bind_button("toggle_crowd", string::KeyCode::K);
    input_map_.bind_button("toggle_lod", string::KeyCode::G);
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

    // Brief 20: the hand-written `usages` vector is GONE. What this pass touches is stated once, in
    // declare(), and that single statement drives both ordering and barrier derivation.

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

        // Shadow sampler + cascade images are allocated by the ShadowMaps component ShadowPass owns.

        // --- Per-frame SceneData SSBO ring (device-addressed, persistent-mapped) ---
        // Brief 16 M1: a registry-owned PerFrame buffer (was a hand-managed [frame] ring of
        // resource_ids + mapped pointers). The registry allocates one physical per frame-in-flight
        // and owns them; resolve address/mapped by slot via resources->{address,mapped}(scene_buffer_, f).

        // Debug controls: T animate sun (time-of-day), [ / ] scrub it, L toggle local lights,
        // H toggle the froxel heatmap.
        input_map_.bind_button("sun_animate", string::KeyCode::T);
        input_map_.bind_button("time_back", string::KeyCode::LEFT_BRACKET);
        input_map_.bind_button("time_fwd", string::KeyCode::RIGHT_BRACKET);
        input_map_.bind_button("toggle_lights", string::KeyCode::L);
        input_map_.bind_button("toggle_heatmap", string::KeyCode::H);
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

// Record the GTAO chain: horizon-search AO + bent normal from the
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
// when the hardware exposes one (timeline edge + queue-family ownership transfer derived from the
// declarations), or records it inline on the main queue otherwise. No Tracy zone here: the
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




geometry_pass::~geometry_pass()
{
    auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    // sky + froxel + IBL programs torn down in their own passes' destructors now (brief 11 step 3).
    destroy_program(meshlet_program_);
    destroy_program(meshlet_twosided_program_);
    destroy_program(hiz_program_);
    destroy_program(reset_program_);
    destroy_program(draw_cull_program_);
    destroy_program(expand_scan_blocks_program_);
    destroy_program(expand_scan_carry_program_);
    destroy_program(expand_fill_program_);
    // Brief 09b probe GI programs.

    // Brief 03 meshlet resources.
    if (meshlet_model_.total_meshlets > 0)
    {
        // Brief 20: the HiZ pyramid and its per-mip slots are graph resources — nothing to unbind.
        // GTAO targets + their bindless slots are released in GtaoPass.s destructor now.
        // stats_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M3).
        // Brief 21 D4: the work lists are graph transients — the graph frees them.
        allocator_.destroy_resource(draw_info_buffer_);
        allocator_.destroy_resource(meshlet_triangles_);
        allocator_.destroy_resource(meshlet_vertices_);
        allocator_.destroy_resource(meshlet_buffer_);
    }

    {
        // Shadow maps + sampler freed in ShadowPass.s destructor now.
        // scene_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M1).
        // light_buffer_ ring is owned + destroyed by the ResourceRegistry now (brief 16 M3).
        // froxel index buffers freed in FroxelPass's destructor now (brief 11 step 3).
    }

    // Brief 07 IBL resources torn down in IblPass's destructor now (brief 11 step 3).
    // Brief 09/09b: GTAO sampler + probe volume were created in the same ctor block as the IBL
    // resources; their own inner guards make the former `env_capture_ != 0` gate unnecessary.
    {
        // Probe GI atlases + sampler freed in GiPass.s destructor now.
    }

    descriptor_table_.unbind(white_image_, ::string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(white_image_);
    descriptor_table_.unbind(flat_normal_image_, ::string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(flat_normal_image_);
    // Only the stb-fallback textures are OURS — the KTX2 ones were created by (and are destroyed by)
    // the TextureStreamer, which we merely cached the id of. texture_streamer_ is a member, so it is
    // destroyed right after this body runs.
    const std::unordered_set<::string::gpu::resource_id> streamed(streamed_textures_.begin(),
                                                                 streamed_textures_.end());
    for (const ::string::gpu::resource_id image : texture_images_)
    {
        if (streamed.contains(image))
        {
            continue;
        }
        descriptor_table_.unbind(image, ::string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(image);
    }

    allocator_.destroy_resource(vertex_buffer_);
}

void geometry_pass::tick(float delta_time, uint32_t current_frame)
{
    // The HiZ dispatch shape and the two-phase decision are CPU state, and they belong HERE rather
    // than inside a recording callback: the graph's toggles are evaluated at execute, which is after
    // tick, so a predicate reading state that a record body sets is reading last frame's answer.
    if (meshlet_program_ != nullptr && draw_info_mapped_ != nullptr)
        ensure_hiz(static_cast<uint16_t>(current_frame));
    // Active only when HiZ is enabled AND geometry is resident (warmup done). When inactive the pass
    // renders single-pass (phase 0), everything unconditionally — exactly like HiZ-off.
    two_phase_active_ = hiz_enabled_ && hiz_program_ != nullptr && stream_frame_ > 1;

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

    // Toggle the debug frozen culling frustum. On freeze, snapshot the current view-projection;
    // the camera keeps moving but the cull test stays against the snapshot, so culled geometry
    // becomes visible as it leaves the frozen view.
    if (input_map_.pressed(debug_actions::freeze_culling))
    {
        mesh_cull_frozen_ = !mesh_cull_frozen_;
        if (mesh_cull_frozen_)
        {
            mesh_frozen_view_proj_ = camera_.view_proj();
            mesh_frozen_camera_pos_ = camera_.position();  // cone/HiZ/LOD eye freezes too
        }
        STRING_LOG_INFO("Cull frustum {}", mesh_cull_frozen_ ? "FROZEN (debug)" : "live");
    }
    if (input_map_.pressed(debug_actions::toggle_culling))
    {
        cull_enabled_ = !cull_enabled_;
        STRING_LOG_INFO("GPU frustum culling {}", cull_enabled_ ? "ON" : "OFF (debug)");
    }

    // --- Brief 03 meshlet-path debug controls ---
    if (input_map_.pressed(debug_actions::toggle_hiz))
    {
        hiz_enabled_ = !hiz_enabled_;
        STRING_LOG_INFO("HiZ occlusion {}", hiz_enabled_ ? "ON" : "OFF");
    }
    if (input_map_.pressed(debug_actions::cycle_debug_view))
    {
        debug_view_ = (debug_view_ + 1) % 4;
        static const char* names[] = { "none", "meshlet-id", "LOD-level", "occlusion-reject" };
        STRING_LOG_INFO("Meshlet debug view: {}", names[debug_view_]);
    }
    if (input_map_.pressed(debug_actions::toggle_lod))
    {
        lod_enabled_ = !lod_enabled_;
        STRING_LOG_INFO("Discrete LOD select {}", lod_enabled_ ? "ON" : "OFF (LOD0)");
    }
    if (input_map_.pressed(debug_actions::toggle_crowd))
    {
        crowd_enabled_ = !crowd_enabled_;
        build_crowd(scene_aabb_min_, scene_aabb_max_);
        STRING_LOG_INFO("Crowd stress scene {}", crowd_enabled_ ? "ON" : "OFF");
    }

    // --- Time-of-day sun + Forward+ debug controls ---
    if (input_map_.pressed(debug_actions::sun_animate))
    {
        sun_animate_ = !sun_animate_;
        STRING_LOG_INFO("Time-of-day {}", sun_animate_ ? "ANIMATING" : "paused");
    }
    if (sun_animate_) time_of_day_ = std::fmod(time_of_day_ + delta_time * 0.03f, 1.0f);
    if (input_map_.held(debug_actions::time_back)) time_of_day_ = glm::clamp(time_of_day_ - delta_time * 0.15f, 0.0f, 1.0f);
    if (input_map_.held(debug_actions::time_fwd))  time_of_day_ = glm::clamp(time_of_day_ + delta_time * 0.15f, 0.0f, 1.0f);
    if (input_map_.pressed(debug_actions::toggle_lights))
    {
        lights_enabled_ = !lights_enabled_;
        STRING_LOG_INFO("Local lights {}", lights_enabled_ ? "ON" : "OFF");
    }
    if (input_map_.pressed(debug_actions::toggle_heatmap))
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
    // The furnace PINS exposure (over auto AND manual) so a radiance-1 environment reads as flat
    // white, not grey. Target is exposed = 4.0, NOT 1.0: filmic transforms map scene 1.0 to only
    // ~80-85% display, so two stops up lands the flat field at display white through both tonemap
    // curves while staying on the shoulder — non-uniformities, the gate's actual signal, stay visible.
    string::composite_pass::set_exposure_override(std::log2(1000.0f / (1.2f * 4.0f)), furnace_);
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

    // Residency feedback — textures and geometry both driven by the SAME per-draw frustum visibility.
    // Textures: pinned to a coarse-tail floor, raised toward each visible draw's screen-coverage mip
    // (finest request wins, aggregated in frame_desired_detail_), and never released. Geometry: only
    // when streaming, want visible draws and release the rest so their heap space is reused.
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
                geometry_residency_->want(d, 1, ::string::gpu::resource_priority::LAZY, stream_frame_);
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
        const auto priority = on_screen ? ::string::gpu::resource_priority::IMMEDIATE
                                        : ::string::gpu::resource_priority::LAZY;
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
        for (const ::string::gpu::resource_id id : streamed_textures_)
        {
            if (residency_.status_of(id) != ::string::gpu::stream_status::CACHED)
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
        // stats_latest_ is refreshed at RECORD time by record_scene_upload (which has the
        // pass_context this tick lacks); here we only publish it.

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
            if (ibl != nullptr)
                STRING_LOG_INFO("[ibl] frame {}: {} env updates so far ({} static sun -> 1 expected)",
                                stream_frame_, ibl->update_count(), sun_animate_ ? "animating" : "");
        }
    }

    ++stream_frame_;
}

// The three resets are three DECLARED PASSES, because they are three producer-consumer stages with
// different consumers: the work lists feed the expand computes and the indirect draws, the stats block
// feeds the cull's histogram atomics, the visibility bits feed the task shaders. As one pass they
// needed two hand-written memory barriers to order themselves against what came next; as declarations
// the graph derives both, and each can be conditioned on its own one-shot predicate.

// Zero every work list ONCE, before anything reads one. They are device-local and therefore
// uninitialised at allocation, and each `commands[]` region feeds draw_mesh_tasks_indirect_count — an
// arbitrary task-group count out of uninitialised memory is a GPU hang, not a wrong picture. Once, not
// once per slot: a transient is single-backed (D3).
void geometry_pass::record_reset_lists(string::pass_context& ctx)
{
    const auto zero = [&](string::gpu::buffer b) {
        if (const ::string::gpu::resource_id id = ctx.id(b); id != 0)
            ctx.rec.fill_buffer(allocator_.get_buffer(id).buffer, 0, VK_WHOLE_SIZE, 0u);
        else
            lists_zeroed_ = false;   // not backed yet — try again next frame
    };
    lists_zeroed_ = true;
    zero(wl_.opaque);
    zero(wl_.twosided);
    zero(wl_.draw_lod);
    for (const string::gpu::buffer& c : wl_.cascade) zero(c);
}

// Zero this frame's stats block. The barrier that used to follow — ordering it before the draw-cull
// compute's histogram atomics — is the cull passes' own declared write of the same buffer.
void geometry_pass::record_reset_stats(string::pass_context& ctx)
{
    const ::string::gpu::pipeline& rp = reset_program_->current();
    const ResetPush rpush{
        .stats = ctx.address(stats_),
        .stats_words = sizeof(GpuMeshStats) / 4,
        ._pad = 0,
    };
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, rp.pipeline);
    ctx.rec.push_constants(rp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ResetPush), &rpush);
    ctx.rec.dispatch(1, 1, 1);
}

// Clear the persistent visibility bitfield + last-LOD on demand (first frame, teleport, a full
// streaming reset). A cleared bitfield is CORRECT — every meshlet takes phase 2 for one frame. Freeze
// (F) must NOT clear: it would erase the frozen visibility the debug eye is inspecting. The barrier
// that used to follow is derived from the task shaders' declared reads of the same buffers.
void geometry_pass::record_reset_visbits(string::pass_context& ctx)
{
    ctx.rec.fill_buffer(allocator_.get_buffer(visbits_buffer_).buffer, 0, VK_WHOLE_SIZE, 0u);
    ctx.rec.fill_buffer(allocator_.get_buffer(prev_draw_lod_buffer_).buffer, 0, VK_WHOLE_SIZE, 0u);
    visbits_clear_pending_ = false;
}


// Brief 11 M2: probe-GI static capture (once) + dynamic relight (amortized). Scheduled by the
// standalone GiPass in the compute prepass — ordered after IblPass (relight's sky-SH source is this
// frame's) and before ShadowPass (the relight->shadow WAR edge). All barriers are internal.
// Returns true if it recorded work.

void geometry_pass::record_phase1(string::pass_context& ctx)
{
    // The froxel buffer's device address is latched HERE rather than in update(), where it used to
    // depend on FroxelPass having updated first — an ordering nothing enforced. By record() time
    // every pass has updated, so this is always the live address. Patched straight into the
    // persistently-mapped SceneData for this frame slot; the GPU has not read it yet (that happens
    // after submit), and this pass is the only consumer of the field.
    //
    // Done BEFORE the draw_count_ early-out: the transparency pass shares this SceneData.
    // Brief 20: the late froxel-address patch is DELETED. It existed because SceneData was filled in
    // update() but the froxel buffer's address was only valid after another pass's update() had run —
    // an ordering nothing enforced, and the one that device-lost the machine. SceneData is now filled
    // in scene.upload at RECORD time, where ctx.address(froxels) is this frame's real address.

    if (draw_count_ == 0)
    {
        return;
    }

    // The procedural sky was drawn by the standalone SkyPass (first pass of this MSAA group) —
    // brief 11 step 3. geometry_pass::record() now starts straight at the meshlet draw path.

    // --- Brief 03/04d: task/mesh meshlet draw path (the scene geometry front-end) ----------------
    if (meshlet_program_ && draw_info_mapped_)
    {
        if (two_phase_active_)
        {
            // Brief 04d PHASE 1: render meshlets marked visible LAST frame (bit set) directly into the
            // MSAA color+depth. No HiZ test (the pyramid isn't built yet). The renderer then MIN-resolves
            // this depth -> hz.depth, the hiz.build pass builds the pyramid, and geometry.phase2 draws
            // phase 2 (the disocclusion complement) + transparency.
            STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
            record_opaque_phase(ctx, /*phase*/ 1u, pyramid_, scene_data_, stats_);
        }
        else
        {
            // Single-pass (HiZ off / warmup): frustum+cone+HiZ, then transparency, all in one render
            // group.
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
                record_opaque_phase(ctx, /*phase*/ 0u, pyramid_, scene_data_, stats_);
            }
            // Brief 09b: probe-debug spheres after opaque. Brief 11 M2: transparency moved OUT to the
            // standalone TransparencyPass (drawn last in the reopened MSAA group — same position).
            // Brief 20: the probe-debug spheres are their own declared pass (gi.debug) now, drawn into
            // the same group by its own declaration rather than called from inside this record.

        }
    }
}

// Brief 04d: draw the opaque one-sided + two-sided camera lists at `phase` (0 legacy / 1 bit-set /
// 2 bit-clear+HiZ+update). Assumes an MSAA scene render pass is already open (record_phase1 for
// phase 1/0, record_phase2 for phase 2). Shared by both phases — the task shader does the partitioning.

// Brief 04d: the depth-resolve target the renderer MIN-resolves the phase-1 MSAA depth into (the
// pyramid's single-sample mip-0 source). Valid only when the two-phase path is active this frame.
// Brief 04d: build the HiZ pyramid from the MIN-resolved phase-1 depth (hz.depth), OUTSIDE rendering.
// The renderer already resolved msaa_depth_ -> hz.depth (reverse-Z MIN = farthest) at phase-1's
// EndRendering and left it in DEPTH_ATTACHMENT layout. Reuses the exact mip-0 uniform-stretch +
// 2x2 min chain the old prepass path used; only the depth SOURCE changed (resolve, not a re-draw).
// Author every pass this subsystem owns. The two-phase occlusion path is three real passes sharing
// this object — phase 1 draws what was visible last frame, the HiZ chain reduces the resolved depth,
// phase 2 draws the disocclusion complement — and the graph orders them from what they declare, not
// from a hook that told the renderer where to break the group.
void geometry_pass::declare(string::frame_graph& fg, string::gpu::image color, string::gpu::image resolve,
                            string::gpu::image depth,
                            string::gpu::image hiz_depth, string::gpu::image hiz_pyramid,
                            const WorklistSet& worklists, string::gpu::buffer scene_data,
                            string::gpu::buffer lights, string::gpu::buffer stats,
                            std::span<const string::gpu::image> cascades, string::gpu::image gtao_ao,
                            string::gpu::image env_prefiltered, string::gpu::image dfg_lut,
                            string::gpu::buffer ibl_sh, string::gpu::buffer froxels)
{
    scene_data_ = scene_data;
    lights_buffer_ = lights;
    stats_ = stats;
    wl_ = worklists;
    pyramid_ = hiz_pyramid;
    gtao_ao_ = gtao_ao;
    env_prefiltered_ = env_prefiltered;
    dfg_lut_ = dfg_lut;
    ibl_sh_ = ibl_sh;
    froxels_ = froxels;
    for (std::size_t c = 0; c < cascades.size() && c < cascades_.size(); ++c) cascades_[c] = cascades[c];

    // The persistent per-meshlet visibility bitfield. This pass allocates and owns the buffer (it is
    // deliberately NOT ring-buffered — the temporal state accumulates across frames), so the graph
    // adopts it rather than allocating it: use_persistent manages usage only.
    //
    // Declaring it is LOAD-BEARING, not bookkeeping. The task shaders read-modify-write these bits
    // every two-phase frame, and storage_write's scope is READ|WRITE, so one declaration on each
    // phase derives BOTH edges that matter: the previous frame's phase-2 writes before this frame's
    // phase-1 reads (the cross-frame RAW — brief 04d's flicker fix), and phase 1 before phase 2
    // within the frame (the WAW/RMW). Without them the bitfield is raced, the "visible last frame"
    // set goes stale or garbage, and phase 2 stops picking up newly disoccluded meshlets — which is
    // invisible while the camera is still and tears the image apart the moment it moves.
    if (visbits_buffer_ != 0)
        visbits_ = fg.use_persistent(string::persistent_buffer_info{
            .name = "meshlet.visbits", .physical = { visbits_buffer_ } });

    // SceneData + the light ring, written before anything reads them. Declaring this is what makes
    // the froxel/shadow-slot addresses inside SceneData safe: they are resolved while this records,
    // against this frame's backing, and every consumer's read is a derived edge against this write.
    string::pass_spec upload = fg.pass("scene.upload");
    upload.writes(scene_data).writes(lights)
          .reads(gtao_ao).reads(env_prefiltered).reads(dfg_lut)
          .reads(ibl_sh).reads(froxels);
    for (std::size_t c = 0; c < cascades.size(); ++c) upload.reads(cascades[c]);
    // COMPUTE, not transfer: this pass records no GPU commands at all (it fills a host-visible ring
    // and resolves slots), so its kind is nominal — but the kind picks the default stage for its
    // reads, and a sampled read at the COPY stage is illegal. Compute is where those slots are
    // legitimately readable.
    upload.compute([this](string::pass_context& ctx) { record_scene_upload(ctx); });

    // Per-frame reset: zero the stats block and, on demand, the persistent visibility bitfield and
    // last-frame LOD. Authored BEFORE the cull so the graph derives reset->cull; without it the cull
    // accumulates into stats nothing cleared.
    // THREE passes, not one: three producer-consumer stages with three different consumers. As one
    // pass they needed two hand-written memory barriers to order themselves against what came next.
    string::pass_spec reset_lists = fg.pass("meshlet.reset.lists");
    reset_lists.writes(wl_.opaque, string::access::transfer_write)
               .writes(wl_.twosided, string::access::transfer_write)
               .writes(wl_.draw_lod, string::access::transfer_write);
    for (const string::gpu::buffer& c : wl_.cascade)
        reset_lists.writes(c, string::access::transfer_write);
    reset_lists.toggle([this] { return !lists_zeroed_; })
               .transfer([this](string::pass_context& ctx) { record_reset_lists(ctx); });

    fg.pass("meshlet.reset.stats")
      .writes(stats)
      .toggle([this] { return reset_program_ != nullptr && draw_count_ > 0; })
      .compute([this](string::pass_context& ctx) { record_reset_stats(ctx); });

    if (visbits_.valid())
    {
        fg.pass("meshlet.reset.visbits")
          .writes(visbits_, string::access::transfer_write)
          .toggle([this] { return visbits_clear_pending_ && !mesh_cull_frozen_; })
          .transfer([this](string::pass_context& ctx) { record_reset_visbits(ctx); });
    }

    // GPU draw-cull + list expansion. Declared by the meshlet TU, which owns those dispatches.
    declare_cull(fg, stats);
    declare_expand(fg);

    // geometry.phase1. It writes the multisampled depth AND the single-sample hz.depth: two depth
    // writes, one multisampled and one not, which IS the declaration that the first resolves into the
    // second (reverse-Z MIN, the conservative choice for a HiZ occluder). That pairing is what
    // replaced Access::DepthResolve.
    string::pass_spec phase1 = fg.pass("geometry.phase1");
    phase1.color(color)
          // The single-sample resolve target. Declaring BOTH a multisampled and a single-sample
          // colour write IS the declaration that the first resolves into the second — see
          // derive_groups.
          .color(resolve)
          .depth(depth)
          .depth(hiz_depth)
          // The camera lists: the indirect commands, and the records[] the task shader indexes.
          .reads(wl_.opaque, string::access::indirect_read)
          .reads(wl_.opaque, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.twosided, string::access::indirect_read)
          .reads(wl_.twosided, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(scene_data)
          .writes(stats);
    // The lit fragments SAMPLE the cascades, GTAO, the IBL products and the froxel list through
    // SceneData's bindless slots — the slots are RESOLVED by scene.upload, but the contents are
    // READ here, at the fragment stage. Declaring the read on the consuming pass is what makes the
    // NEXT frame's producer rewrite derive a write-after-read edge against THIS frame's in-flight
    // fragment work. scene.upload's compute-stage reads cannot stand in for that: a barrier whose
    // source scope is COMPUTE does not wait for fragment sampling still in flight, which is why the
    // shadow term flickered run-to-run — the cascade was rewritten mid-read. (This is the edge the
    // deleted per-frame shadow/GTAO rings used to paper over with triple buffering.)
    const auto lit_reads = [&](string::pass_spec& p) {
        for (const string::gpu::image& c : cascades) p.reads(c);
        p.reads(gtao_ao).reads(env_prefiltered).reads(dfg_lut).reads(ibl_sh).reads(froxels);
    };
    lit_reads(phase1);
    if (visbits_.valid())
        phase1.writes(visbits_, string::access::storage_write, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    phase1.toggle([] { return cv_pass_geometry().get(); })
          .raster([this](string::pass_context& ctx) { record_phase1(ctx); });

    declare_hiz(fg, hiz_depth, hiz_pyramid);

    // geometry.phase2 reads the pyramid at the TASK stage — genuinely ambiguous, so it says so — and
    // re-declares the attachments, which is what re-forms the reloaded MSAA group.
    string::pass_spec phase2 = fg.pass("geometry.phase2");
    phase2.color(color)
          .depth(depth)
          .reads(hiz_pyramid, string::access::sampled_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.opaque, string::access::indirect_read)
          .reads(wl_.opaque, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.twosided, string::access::indirect_read)
          .reads(wl_.twosided, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    lit_reads(phase2);   // phase-2 fragments shade exactly like phase 1's — same consumed set
    if (visbits_.valid())
        phase2.writes(visbits_, string::access::storage_write, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    phase2.toggle([this] { return two_phase_active_ && cv_pass_geometry().get(); })
          .raster([this](string::pass_context& ctx) { record_phase2(ctx); });
}

// Brief 20: the HiZ reduction is one DECLARED PASS PER MIP, not a loop with hand-rolled barriers.
// Mip 0 reduces the resolved scene depth; every later mip reduces the one above it. Because a slice
// is a first-class declaration, `.reads(pyramid.mip(m-1))` and `.writes(pyramid.mip(m))` do not
// collide, so the graph derives the chain — and derives ONLY the chain, rather than serialising the
// whole image the way a single-state tracker had to.
// The chain is authored at kMaxHizMips and each level asks, every frame, whether the CURRENT
// viewport's pyramid actually reaches it. That is the compiled-once answer to a variable-length
// chain: the declaration count is fixed at author time, and the window decides which links survive.
void geometry_pass::declare_hiz(string::frame_graph& fg, string::gpu::image depth,
                                string::gpu::image pyramid)
{
    for (uint32_t m = 0; m < kMaxHizMips; ++m)
    {
        string::pass_spec spec = fg.pass("hiz.mip" + std::to_string(m));
        if (m == 0) spec.reads(depth);
        else        spec.reads(pyramid.mip(m - 1));
        spec.writes(pyramid.mip(m))
            .toggle([this, m] {
                return two_phase_active_ && hiz_program_ != nullptr
                    && m < hiz_mip_count(screen_size);
            })
            .compute([this, m, depth, pyramid](string::pass_context& ctx) {
                record_hiz_mip(ctx, m, depth, pyramid);
            });
    }
}

// One mip's reduction. No barrier: the declaration above is what orders this against the mip before
// it, and against the depth resolve that produced mip 0's source.
void geometry_pass::record_hiz_mip(string::pass_context& ctx, uint32_t m, string::gpu::image depth,
                                   string::gpu::image pyramid)
{
    const HizPyramid& hz = hiz_[ctx.frame_slot];
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
    const ::string::gpu::pipeline& hp = hiz_program_->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline);
    VkDescriptorSet set = descriptor_table_.get_set();
    ctx.rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline_layout,
                                 0, 1, &set, 0, nullptr);
    const uint32_t dw = std::max(1u, hz.size.x >> m);
    const uint32_t dh = std::max(1u, hz.size.y >> m);
    HizPush hpush{};
    if (m == 0)
    {
        hpush.src_slot = ctx.slot(depth);
        hpush.src_size = glm::uvec2(screen_size.width, screen_size.height);
        hpush.copy_depth = 1;
        hpush.src_level = 0;   // the scene-depth image has a single level
    }
    else
    {
        hpush.src_slot = ctx.slot(pyramid);   // whole-chain view; src_level selects the mip
        hpush.src_size = glm::uvec2(std::max(1u, hz.size.x >> (m - 1)), std::max(1u, hz.size.y >> (m - 1)));
        hpush.copy_depth = 0;
        hpush.src_level = m - 1;
    }
    hpush.dst_slot = ctx.slot(pyramid.mip(m));
    hpush.dst_size = glm::uvec2(dw, dh);
    ctx.rec.push_constants(hp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(HizPush), &hpush);
    ctx.rec.dispatch((dw + 7) / 8, (dh + 7) / 8, 1);

    // NO barrier here, and none at the end of the chain.
    //
    // The per-mip compute->compute edge is derived from mip m's write against mip m+1's declared
    // read. The pyramid's GENERAL->SHADER_READ handoff to phase 2's task shader is derived from
    // phase 2's declared read. And the hz.depth DEPTH_ATTACHMENT restore is gone: that barrier
    // existed because the tracker forgot everything at the frame boundary, so a resource consumed
    // one frame LATE by GTAO reprojection had to be hand-parked in its resting layout. Tracked
    // state now carries across the boundary, per slot, against that slot's own image — so the
    // cross-frame edge is derived like any other. This was the one exception in the whole renderer
    // that passed the "before the graph exists" test, and it passed it only because nothing
    // modelled it.

    // Brief 09: this slot's hz.depth now holds this frame's resolved phase-1 depth — capture the
    // matrices GTAO will reproject through when it consumes this slot NEXT frame. Rendering uses
    // the LIVE camera even under freeze-cull, so these are the true depth-buffer transforms.
    // Only on the last mip, so it happens once per frame rather than once per dispatch.
    if (m + 1 == hz.mips && ctx.frame_slot < depth_history_.size())
    {
        DepthHistorySlot& h = depth_history_[ctx.frame_slot];
        h.valid = 1;
        h.view = camera_.view();
        h.view_proj = camera_.view_proj();
        h.proj = camera_.view_proj() * glm::inverse(camera_.view());
    }
}

// Brief 04d PHASE 2: recorded INSIDE the reopened MSAA group (which LOADed phase-1's color+depth).
// Renders the disocclusion complement (bit-clear meshlets that the fresh pyramid says are visible),
// sets their bits, then the sorted transparency pass (tests vs the final opaque depth, as before).
void geometry_pass::record_phase2(string::pass_context& ctx)
{
    // Brief 11 step 2: this is now an ALWAYS-scheduled pass (geometry.phase2). In single-pass mode
    // record() already drew the disocclusion-free opaque + probe + transparency, so phase 2 must not
    // run — else it double-draws. Only the two-phase path uses it.
    if (!two_phase_active_ || !meshlet_program_ || !draw_info_mapped_) return;
    record_opaque_phase(ctx, /*phase*/ 2u, pyramid_, scene_data_, stats_);
    // Brief 09b: probe-debug spheres after opaque, depth-tested. Brief 11 M2: transparency moved OUT to
    // the standalone TransparencyPass, which draws last in this same reopened MSAA group (byte-identical).
    // Brief 20: gi.debug is a declared pass; nothing to call from here.
}


// Fill this frame's SceneData + light ring. A declared pass, because it WRITES two buffers the
// lit draws read — so the graph derives that edge instead of it resting on update() ordering.
void geometry_pass::record_scene_upload(string::pass_context& ctx)
{
    const uint32_t current_frame = ctx.frame_slot;

    // Read back the stats the GPU wrote when this slot last ran (frames_in_flight frames ago —
    // its fence passed in begin_frame, so no stall). This must happen at RECORD time because only
    // pass_context can resolve the slot's mapped pointer; tick() publishes the value it finds in
    // stats_latest_. (Restores the readback the brief-20 rewrite stubbed out — the HUD, the
    // inspector and every culling number were silently zero without it.)
    if (stats_.valid() && draw_count_ > 0)
    {
        if (const void* stats_mapped = ctx.mapped(stats_))
        {
            std::memcpy(&stats_latest_, stats_mapped, sizeof(GpuMeshStats));
            // STRING_STATS_LOG=1: periodic cull-funnel dump, for diagnosing meshlet loss without a
            // GPU capture — which stage kills geometry shows up directly in the funnel.
            static const bool log_stats = std::getenv("STRING_STATS_LOG") != nullptr;
            static uint32_t stats_log_frame = 0;
            if (log_stats && (++stats_log_frame % 60u) == 0u)
            {
                const GpuMeshStats& s = stats_latest_;
                STRING_LOG_INFO("[stats] f{} total {} frustum {} cone {} hiz(drawn) {} phase2 {} "
                                "shadow {} tris {}",
                                stats_log_frame, s.meshlets_total, s.after_frustum, s.after_cone,
                                s.after_hiz, s.phase2_drawn, s.shadow_draws, s.triangles);
            }
        }
    }
    // --- Forward+ per-frame GPU data (this frame's ring slot) ----------------------------------
    if (draw_count_ > 0 && scene_data_.valid() && current_frame < frames_in_flight_)
    {
        // froxel index-buffer capacity is ensured in FroxelPass::update now (brief 11 step 3).

        // Upload this frame's animated lights (or none, if the stress set is toggled off).
        const uint32_t light_count = lights_enabled_ ? static_cast<uint32_t>(lights_.size()) : 0u;
        if (light_count > 0)
        {
            std::memcpy(ctx.mapped(lights_buffer_), lights_.data(),
                        sizeof(GpuLight) * light_count);
        }

        // Fill SceneData for this frame. The lit shader reads sun/ambient/CSM/froxel state from here.
        SceneData scene{};
        scene.camera_pos = camera_.position();
        scene.exposure = string::composite_pass::exposure_scale();   // for display-referred debug views
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
            scene.cascade_slot[c] = glm::uvec4(shadow != nullptr ? ctx.slot(cascades_[c]) : 0u, 0, 0, 0);
        }
        // No shadow-off special case. The cascades are TRANSIENTS declaring `neutral = 1.0`, so when
        // the cascade passes are toggled off ctx.slot() above already resolves to the neutral
        // fallback and the shadow term reads unshadowed — the graph's degrade, not a second one
        // written by hand in the data (brief 21 step 4).
        scene.cascade_count = settings_.cascade_count;
        // STRING_STATS_LOG: one-shot cascade slot identity — distinct slots per cascade proves the
        // consumers resolve three real maps; equal slots proves a collapse (fallback or slot mixup).
        {
            static const bool log_slots = std::getenv("STRING_STATS_LOG") != nullptr;
            static bool slots_logged = false;
            if (log_slots && !slots_logged)
            {
                slots_logged = true;
                STRING_LOG_INFO("[slots] cascade slots {} {} {} count {} | gtao {} env {} dfg {}",
                                scene.cascade_slot[0].x, scene.cascade_slot[1].x,
                                scene.cascade_slot[2].x, scene.cascade_count, scene.gtao_slot,
                                scene.env_slot, scene.dfg_slot);
            }
        }
        scene.shadow_texel = 1.0f / static_cast<float>(settings_.shadow_resolution);
        scene.shadow_bias = cv_shadow_bias().get();
        scene.shadow_normal_offset_scale = cv_shadow_normal_offset().get();
        scene.cascade_blend = settings_.cascade_blend;
        scene.view = camera_.view();
        // froxel grid dims + buffer address come from FroxelPass's component, published in the scene.
        const uint32_t froxel_tx = froxel ? froxel->tiles_x(ctx.extent) : 0;
        const uint32_t froxel_ty = froxel ? froxel->tiles_y(ctx.extent) : 0;
        scene.froxel_dims = glm::uvec4(froxel_tx, froxel_ty, kFroxelDepthSlices, kFroxelTileSize);
        const float near_p = camera_.near_plane();
        const float far_p = std::min(settings_.shadow_depth_range, camera_.far_plane());
        scene.froxel_planes = glm::vec4(near_p, far_p, 1.0f / std::log(far_p / near_p),
                                        static_cast<float>(light_count));
        scene.lights = light_count > 0 ? ctx.address(lights_buffer_) : 0;
        // The froxel list's address. This pass DECLARES it (`.reads(froxels)` above), so resolving it
        // here is exactly what the declaration licenses — the address is this frame's backing, at
        // record time, ordered against the froxel pass by the graph.
        //
        // This MUST be set: `lighting.slang` dereferences it the moment a scene has local lights
        // (`froxel_planes.w > 0`), and a null device address is a GPU fault, not a wrong colour.
        scene.froxels = ctx.address(froxels_);
        scene.max_lights_per_froxel = kMaxLightsPerFroxel;
        scene.debug_flags = (froxel_heatmap_ ? 1u : 0u) | (furnace_ ? 2u : 0u)
                          | (cv_gtao_spec_occ().get() ? 0u : 4u)    // bit2: disable bent-normal spec-occ
                          | ((static_cast<uint32_t>(std::max(cv_light_debug().get(), 0)) & 0xFu) << 4);  // bits4-7: lighting isolate
        // Brief 07: the sky-IBL products (single-buffered; the update chain is ordered against
        // in-flight readers by the declared SH usages).
        scene.sh = ctx.address(ibl_sh_);
        scene.env_slot = ctx.slot(env_prefiltered_);
        scene.env_mips = ibl->env_mips();
        scene.dfg_slot = ctx.slot(dfg_lut_);

        // Ensure the HiZ pyramid HERE, in update() — its (re)build binds new
        // per-mip storage views into the bindless set, and now that IblPass records its compute BEFORE
        // this pass, that descriptor update must land in the update phase (before any set bind this
        // frame), not at record time. ensure_hiz is idempotent, so the later calls no-op.
        if (meshlet_program_ && draw_info_mapped_)
            ensure_hiz(current_frame);
        // GtaoPass decided the go/no-go in its own update (it runs before this pass); read it here.
        const bool gtao_runs = gtao != nullptr && gtao->runs();
        const uint16_t gtao_prev = gtao != nullptr ? gtao->prev_slot(current_frame) : 0;
        scene.prev_view_proj = gtao_runs && gtao_prev < depth_history_.size()
            ? depth_history_[gtao_prev].view_proj : glm::mat4(1.0f);
        // The ~0u sentinel is gtao's ONLY sound degrade — the shader branches to the clean no-AO
        // path. A neutral texture cannot stand in: gtao.ao holds an ENCODED bent normal, and any
        // constant decodes to a degenerate vector (0.5s -> the zero vector, which collapsed the
        // sun/spec terms into the giant black regions that presented as broken shadows).
        scene.gtao_slot = gtao != nullptr && gtao->has_output() ? ctx.slot(gtao_ao_) : 0xFFFFFFFFu;
        scene.gtao_strength = std::clamp(cv_gtao_strength().get(), 0.0f, 1.0f);
        const glm::uvec2 gtao_extent = gtao != nullptr ? gtao->size() : glm::uvec2(0);
        scene.gtao_w = gtao_extent.x;   // half-res AO extent for the lit shader.s joint upsample
        scene.gtao_h = gtao_extent.y;

        // Brief 09b probe GI: the component owns the gate + volume + atlas slots.
        if (gi != nullptr) gi->fill_scene_data(scene, ctx); else scene.probe_gi = 0u;

        std::memcpy(ctx.mapped(scene_data_), &scene, sizeof(SceneData));
    }
}

}  // namespace string::render
