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
#include <vector>

#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include "geometry_pass.hpp"
#include <glm/gtc/constants.hpp>
#include <string/gpu/pipeline_builder.hpp>
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

}  // namespace

GeometryPass::GeometryPass(PassContext& context, const std::filesystem::path& model_path,
                           std::shared_ptr<MeshOverlayStats> overlay_stats)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, frames_in_flight_(context.frames_in_flight)
, residency_(kTextureBudget, kTextureStreamPerFrame)
, overlay_stats_(std::move(overlay_stats))
{
    // Parse the glTF (fast: buffers + resolved texture sources + materials), then overlap the
    // two expensive stages: texture decode runs on the worker pool while this thread flattens
    // the geometry. Texture decode dominates load, so hiding the flatten (and the vertex/index
    // upload) under it is effectively free.
    const auto load_start = std::chrono::steady_clock::now();
    GltfParsed parsed = parse_gltf(context.resources_path / model_path);

    // Kick off the uncooked (stb) texture decodes now, before flattening — they run on the pool
    // while the main thread flattens geometry. Cooked .ktx2 textures are NOT touched here: their
    // UASTC->BC7 transcode is deferred to the streamer's background workers on first use (it's the
    // dominant cost), so at load they only get a header read + a placeholder slot.
    string::core::job_system decode_pool;
    std::vector<std::optional<std::filesystem::path>> ktx_paths(parsed.textures.size());
    std::vector<std::future<DecodedTexture>> decode_jobs(parsed.textures.size());
    for (std::size_t i = 0; i < parsed.textures.size(); ++i)
    {
        ktx_paths[i] = ktx_sibling(parsed.textures[i]);
        if (!ktx_paths[i])
        {
            const GltfTexture* source = &parsed.textures[i];
            decode_jobs[i] = decode_pool.enqueue([source]() { return decode_texture(*source); });
        }
    }

    // Flatten geometry on this thread (overlaps the decode jobs above). Non-const so its vertex /
    // index arrays can be moved into the geometry streamer (which uploads them on demand).
    GltfGeometry geometry = flatten_geometry(parsed);
    materials_ = parsed.materials;
    draws_ = geometry.draws;

    STRING_LOG_INFO("glTF loaded: {} vertices, {} indices, {} draws, {} materials, {} textures",
                    geometry.vertices.size(), geometry.indices.size(), geometry.draws.size(),
                    parsed.materials.size(), parsed.textures.size());

    // Heap capacities (in elements). The streamer suballocates a SEPARATE range per draw, so shared
    // vertices (instanced primitives) are duplicated — size to the sum of per-draw ranges, not the
    // deduplicated vertex count, or everything can't fit even at 100%. kGeometryResidentPercent < 100
    // caps below that to exercise reclaim (with pop-in / possible thrash when the visible set exceeds
    // the budget); 100 keeps everything resident (reclaim still happens for geometry left behind as
    // you look around, freeing ranges — visible in the [geo] logs — but no artifacts).
    uint64_t vertex_units = 0;
    for (const GltfDraw& d : draws_)
    {
        if (d.index_count == 0) continue;
        uint32_t vmin = std::numeric_limits<uint32_t>::max();
        uint32_t vmax = 0;
        for (uint32_t k = 0; k < d.index_count; ++k)
        {
            const uint32_t v = geometry.indices[d.index_offset + k];
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }
        vertex_units += (vmax - vmin + 1);
    }
    const uint64_t index_units = geometry.indices.size();  // indices aren't shared across draws
    const uint64_t vertex_capacity = std::max<uint64_t>(1, vertex_units * kGeometryResidentPercent / 100);
    const uint64_t index_capacity = std::max<uint64_t>(1, index_units * kGeometryResidentPercent / 100);
    const uint32_t vertex_size = static_cast<uint32_t>(sizeof(String::Vertex) * vertex_capacity);
    const uint32_t index_size = static_cast<uint32_t>(sizeof(uint32_t) * index_capacity);

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

    index_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
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

    // Collect textures: cooked KTX2 register with the streamer (BC7 image + placeholder slot now,
    // mips streamed in on demand from the coarse tail up); stb fallbacks upload whole here.
    texture_streamer_ = std::make_unique<TextureStreamer>(
        device_, allocator_, descriptor_table_, context.transfer, white.view, white.sampler);
    texture_images_.resize(parsed.textures.size());
    texture_slots_.resize(parsed.textures.size());
    texture_lod_.resize(parsed.textures.size());
    frame_desired_detail_.resize(parsed.textures.size(), 0);
    for (std::size_t i = 0; i < parsed.textures.size(); ++i)
    {
        if (ktx_paths[i])
        {
            const TextureStreamer::Registered reg =
                texture_streamer_->add(*ktx_paths[i], parsed.textures[i].srgb);
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
                    parsed.textures.size(), decode_pool.worker_count(), load_ms);
    STRING_LOG_INFO("[stream] {} of {} textures streamed (BC7, coarse tail first); rest stb",
                    texture_streamer_->count(), parsed.textures.size());
    // The renderer drains the transfer batch (wait_idle) once after all passes are built; no
    // per-pass flush needed here.

    // --- GPU-driven draw data ------------------------------------------------------------------
    draw_count_ = static_cast<uint32_t>(draws_.size());
    if (draw_count_ > 0)
    {
        // Brief 03: meshletize the model (per-draw LOD chain + meshlets) BEFORE the streamer takes
        // the geometry arrays. Content-hash disk-cached. The GPU meshlet buffers are uploaded in
        // build_meshlet_gpu() below; the DrawInfo table's material/transform fields are filled there
        // too (the builder produces geometry-only draw records).
        {
            const std::filesystem::path meshlet_cache =
                std::filesystem::temp_directory_path() / "string-meshlet-cache";
            const uint64_t seed = std::hash<std::string>{}(model_path.string());
            meshlet_model_ = build_meshlets(geometry.vertices, geometry.indices, draws_,
                                            meshlet_cache, seed);
            STRING_LOG_INFO("[meshlet] {} draws -> {} meshlets, {} vtx-remap, {} tri-words ({:.1f} ms, {})",
                            meshlet_model_.draws.size(), meshlet_model_.total_meshlets,
                            meshlet_model_.meshlet_vertices.size(), meshlet_model_.meshlet_triangles.size(),
                            meshlet_model_.build_ms, meshlet_model_.from_cache ? "cache hit" : "built");
        }

        // Geometry streaming: hand the CPU geometry to the streamer (suballocates + uploads per-draw
        // ranges on demand, frees on eviction) and register every draw with the geometry residency
        // manager. The manager's byte budget matches the heap capacity so it evicts (reclaims) once
        // the heaps are full. On residency the streamer flips the draw's DrawInfo gate (resident +
        // vertex_offset), revealing it; on eviction it clears the resident flag.
        geometry_streamer_ = std::make_unique<GeometryStreamer>(
            allocator_, context.transfer, vertex_buffer_, index_buffer_,
            std::move(geometry.vertices), std::move(geometry.indices),
            vertex_capacity, index_capacity, frames_in_flight_);
        geometry_streamer_->set_draws(draws_);
        geometry_streamer_->set_residency_callback(
            [this](std::uint32_t draw, std::uint32_t vertex_offset, std::uint32_t resident) {
                // Streaming seam: DrawInfo.resident is the ONLY draw gate. vertex_offset (heap delta)
                // rides along — meshlet-vertices hold ORIGINAL global indices and the mesh shaders
                // rebase them into the suballocated heap.
                if (draw_info_mapped_)
                {
                    draw_info_mapped_[draw].resident = resident;
                    draw_info_mapped_[draw].vertex_offset = vertex_offset;
                }
            });

        // Build the GPU meshlet buffers + the DrawInfo table (host-visible resident gate) now, so the
        // up-front streaming loop below can flip DrawInfo.resident via the callback.
        build_meshlet_gpu(context);

        geometry_streaming_ = kGeometryResidentPercent < 100;
        if (geometry_streaming_)
        {
            // Doesn't fit: register with the manager and stream per-frame by visibility (update()).
            const VkDeviceSize geometry_budget = VkDeviceSize(vertex_capacity) * sizeof(String::Vertex)
                                               + VkDeviceSize(index_capacity) * sizeof(uint32_t);
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

    // Headless-debug env overrides (used with STRING_CAPTURE_FRAME for agent-driven A/B runs):
    // STRING_HIZ=0, STRING_LOD=0, STRING_CULL=0, STRING_CAM="px,py,pz,yaw,pitch" (radians).
    if (const char* p = std::getenv("STRING_HIZ"); p && p[0] == '0') hiz_enabled_ = false;
    if (const char* p = std::getenv("STRING_LOD"); p && p[0] == '0') lod_enabled_ = false;
    if (const char* p = std::getenv("STRING_CULL"); p && p[0] == '0') cull_enabled_ = false;
    if (const char* p = std::getenv("STRING_LIGHTS"); p && p[0] == '0') lights_enabled_ = false;
    if (const char* c = std::getenv("STRING_CAM"))
    {
        glm::vec3 pos{}; float yaw = 0.0f, pitch = 0.0f;
        if (std::sscanf(c, "%f,%f,%f,%f,%f", &pos.x, &pos.y, &pos.z, &yaw, &pitch) == 5)
            camera_.set_pose(pos, yaw, pitch);
    }
    if (const char* p = std::getenv("STRING_VIEW")) debug_view_ = std::atoi(p);
    // STRING_CROWD=1 triggers the K-toggle crowd stress path at first update (headless benchmark).
    // The build is deferred to update() (needs residency known); update()'s lazy-build handles it.
    if (const char* p = std::getenv("STRING_CROWD"); p && p[0] == '1') crowd_enabled_ = true;
    // (STRING_DRAW_MIN/MAX bisection removed with brief 03b: draws are now GPU-generated into one
    //  indirect list, so a CPU draw-index window no longer maps to the dispatch loop.)

    // Declared graph usages: the shared buffers + the render targets. The uploaded textures are
    // static SHADER_READ_ONLY inputs (transitioned once by the transfer batch), so they're not
    // graph-tracked and don't need declaring here.
    usages = {
        { vertex_buffer_,       Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { index_buffer_,        Access::IndexRead,   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        { context.depth_target, Access::DepthWrite,  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT },
    };
    // The indirect buffer's DRAW_INDIRECT read is covered by the transfer batch's global barrier
    // (like the static textures); once buffer state is graph-tracked it can be declared here too.

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

        // Debug controls: T animate sun (time-of-day), [ / ] scrub it, L toggle local lights,
        // H toggle the froxel heatmap.
        input_map_.bind_button("sun_animate", String::KeyCode::T);
        input_map_.bind_button("time_back", String::KeyCode::LEFT_BRACKET);
        input_map_.bind_button("time_fwd", String::KeyCode::RIGHT_BRACKET);
        input_map_.bind_button("toggle_lights", String::KeyCode::L);
        input_map_.bind_button("toggle_heatmap", String::KeyCode::H);
    }
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
    if (const char* env = std::getenv("STRING_MESHLET_READBACK"); env && env[0] == '1')
    {
        meshlet_readback_pending_ = true;
    }
    if (const char* env = std::getenv("STRING_MESHLET_DUMP"))
    {
        const uint32_t d0 = uint32_t(std::atoi(env));
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

    // --- Brief 03b: GPU draw-cull work lists (per frame in flight). Each buffer packs commands[]
    // (12B stride), then records[] (8B), then a count uint. records[] aligned to 16B for a clean
    // device-address SSBO base. INDIRECT_BUFFER usage lets the driver read commands[] + count.
    cull_max_draws_ = max_draws;
    const VkDeviceSize cmd_bytes = VkDeviceSize(sizeof(uint32_t) * 3) * max_draws;   // 12B/cmd
    cull_shadow_cmds_offset_ = (cmd_bytes + 15) & ~VkDeviceSize(15);
    cull_records_offset_ = (cull_shadow_cmds_offset_ + cmd_bytes + 15) & ~VkDeviceSize(15);
    const VkDeviceSize rec_bytes = VkDeviceSize(sizeof(uint32_t) * 2) * max_draws;   // 8B/record
    cull_count_offset_ = (cull_records_offset_ + rec_bytes + 15) & ~VkDeviceSize(15);
    const VkDeviceSize worklist_size = cull_count_offset_ + 16;
    cull_cmd_buffers_.resize(frames_in_flight_);
    cull_prepass_buffers_.resize(frames_in_flight_);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        const auto make_worklist = [&]() {
            return allocator_.create_resource(string::gpu::buffer_info{
                .size = worklist_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                       | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                .allocation_flags = {},
            });
        };
        cull_cmd_buffers_[f] = make_worklist();
        cull_prepass_buffers_[f] = make_worklist();
    }

    // --- Pipelines (all through the hot-reload registry) ---
    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    VkSampleCountFlagBits samples = context.sample_count;

    // Task/mesh/fragment lit meshlet pipeline. Same fixed state as the lit vertex pipeline (back-face
    // cull, CCW, MSAA, reverse-Z depth, alpha blend), but built via build_mesh_pipeline (no VI state).
    meshlet_program_ = context.shader_registry.create(
        context.resources_path / "shaders" / "meshlet_mesh.slang",
        [layout, samples](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
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
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_multisampling(samples)
                .enable_depth_stencil()
                .enable_color_blending()
                .build_mesh_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

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
    hiz_program_ = make_compute("hiz_build.slang");
    reset_program_ = make_compute("meshlet_reset.slang");
    draw_cull_program_ = make_compute("meshlet_draw_cull.slang");

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
}

// Brief 03b: dispatch the GPU draw-cull compute to build a work list (commands[] + records[] + count)
// for one consumer. `prepass` forces LOD0 (and skips the LOD histogram) for the HiZ depth prepass;
// the camera list (prepass=false) selects LODs and writes the histogram, and is reused by the main
// pass + all 3 shadow cascades. The list buffer's count is zeroed here, then the compute appends.
// Leaves a barrier making commands[]/records[]/count visible to DRAW_INDIRECT + task-shader reads.
void GeometryPass::record_draw_cull(VkCommandBuffer cb, uint16_t current_frame, bool prepass)
{
    const string::gpu::resource_id list = prepass ? cull_prepass_buffers_[current_frame]
                                                   : cull_cmd_buffers_[current_frame];
    const VkDeviceAddress base = allocator_.get_buffer(list).device_address;

    // No count pre-clear needed: the compute writes commands[]/records[] at a stable slot per draw
    // (== draw index) and the last thread publishes count = active_draw_count_ (skipped draws get a
    // groupCountX=0 command). This keeps the submission order identical to the old per-draw loop.
    float lod_error_px = 0.02f;
    if (const char* t = std::getenv("STRING_LOD_PX")) lod_error_px = float(std::atof(t));
    const float half_h = screen_size.height * 0.5f;
    const float focal = half_h / std::tan(glm::radians(camera_.fov_degrees() * 0.5f));

    const string::gpu::pipeline& cp = draw_cull_program_->current();
    DrawCullPush cull{};
    // Freeze-aware inputs: when F is held, EVERY camera-derived cull input comes from the frozen
    // pose (frustum planes, LOD eye) so flying the free camera reveals exactly what the frozen
    // camera would draw — no mixed live/frozen artifacts.
    cull.cull_view_proj = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : camera_.view_proj();
    cull.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    cull.commands = base;
    cull.shadow_commands = base + cull_shadow_cmds_offset_;
    cull.records = base + cull_records_offset_;
    cull.count = base + cull_count_offset_;
    cull.stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address;
    cull.draw_count = active_draw_count_;
    cull.lod_enabled = (!prepass && lod_enabled_) ? 1u : 0u;   // prepass forces LOD0
    cull.frustum_cull = cull_enabled_ ? 1u : 0u;               // C toggle disables draw-level cull
    cull.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    cull.lod_error_px = lod_error_px;
    cull.focal = focal;
    cull.stats_lod = prepass ? 0u : 1u;                        // histogram: camera list only
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp.pipeline);
    vkCmdPushConstants(cb, cp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(DrawCullPush), &cull);
    vkCmdDispatch(cb, (active_draw_count_ + 63) / 64, 1, 1);

    // Make the work list visible to the indirect draw (count + commands) and the task shader (records).
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

// Brief 03b: the main pass draws the camera work list with ONE vkCmdDrawMeshTasksIndirectCountEXT.
// The list (commands[] + records[]) was built by record_draw_cull earlier this frame; the task shader
// reads its {draw_index, lod} via SV_DrawIndex. groupCountX per command = ceil(LOD meshlets / 32).
void GeometryPass::record_meshlet_draws(VkCommandBuffer cb, const string::gpu::pipeline& p,
                                        uint16_t current_frame)
{
    const glm::mat4 vp = camera_.view_proj();
    const glm::mat4 cull_vp = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : vp;
    const HizPyramid& hz = hiz_[current_frame];
    const bool hiz_ready = hiz_enabled_ && hz.image != 0;
    const string::gpu::resource_id list = cull_cmd_buffers_[current_frame];
    const VkDeviceAddress base = allocator_.get_buffer(list).device_address;

    MeshletPush push{};
    push.view_proj = vp;
    push.cull_view_proj = cull_enabled_ ? cull_vp : vp;   // (frustum planes; C disables via wide test below)
    push.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
    push.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
    push.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
    push.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
    push.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
    push.scene = allocator_.get_buffer(scene_buffers_[current_frame]).device_address;
    push.stats = allocator_.get_buffer(stats_buffers_[current_frame]).device_address;
    push.records = base + cull_records_offset_;
    // Frozen-aware eye: cone backface + HiZ nearest-point tests must use the SAME eye the frustum
    // froze from, or freeze-cull mixes live/frozen inputs (visible as bogus culling when flying).
    push.camera_pos = mesh_cull_frozen_ ? mesh_frozen_camera_pos_ : camera_.position();
    push.debug_view = static_cast<uint32_t>(debug_view_);
    push.hiz_slot = hiz_ready ? hz.sample_slot : 0;
    push.hiz_mips = hiz_ready ? hz.mips : 0;
    push.hiz_size = hiz_ready ? hz.size : glm::uvec2(1, 1);
    const VkBuffer buf = allocator_.get_buffer(list).buffer;

    vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletPush), &push);
    vkCmdDrawMeshTasksIndirectCountEXT(cb, buf, /*offset*/ 0, buf, cull_count_offset_,
                                       cull_max_draws_, sizeof(uint32_t) * 3);
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
        // cascade covers [last_split, split] of the camera's linear depth.
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec3 ray = corners[i * 2 + 1] - corners[i * 2 + 0];  // far -> near direction
            const glm::vec3 near_corner = corners[i * 2 + 0] + ray * ((last_split - near_clip) / range);
            const glm::vec3 far_corner = corners[i * 2 + 0] + ray * ((split - near_clip) / range);
            corners[i * 2 + 0] = far_corner;
            corners[i * 2 + 1] = near_corner;
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners) center += corner;
        center /= 8.0f;

        float radius = 0.0f;
        for (const glm::vec3& corner : corners) radius = std::max(radius, glm::length(corner - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantize the radius so it doesn't jitter

        const float texel = (2.0f * radius) / static_cast<float>(settings_.shadow_resolution);
        cascade_world_texel_[c] = texel;

        const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 eye = center + L * radius;
        glm::mat4 view = glm::lookAt(eye, center, up);

        // Snap the center to whole texels in light space (stabilization).
        glm::vec3 center_ls = glm::vec3(view * glm::vec4(center, 1.0f));
        center_ls.x = std::floor(center_ls.x / texel) * texel;
        center_ls.y = std::floor(center_ls.y / texel) * texel;
        const glm::vec3 snapped_center = glm::vec3(glm::inverse(view) * glm::vec4(center_ls, 1.0f));
        const glm::vec3 snapped_eye = snapped_center + L * radius;
        view = glm::lookAt(snapped_eye, snapped_center, up);

        // Ortho box: [-radius, radius] in x/y; depth spans the sphere plus a pull-back for off-frustum
        // occluders (a tall pillar casting into the slice from outside it).
        const float depth_pad = radius * 3.0f;
        glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + depth_pad);
        proj[1][1] *= -1.0f;  // Vulkan Y-flip
        glm::mat4 reverse_z(1.0f);
        reverse_z[2][2] = -1.0f;
        reverse_z[3][2] = 1.0f;
        proj = reverse_z * proj;

        cascade_view_proj_[c] = proj * view;
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
    s.sun_intensity = glm::mix(1.6f, 3.2f, noon);
    s.sky_zenith = glm::mix(glm::vec3(0.06f, 0.10f, 0.24f), glm::vec3(0.14f, 0.30f, 0.62f), noon);
    s.sky_ground = glm::mix(glm::vec3(0.10f, 0.07f, 0.06f), glm::vec3(0.22f, 0.19f, 0.15f), noon);
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
        L.color_intensity = glm::vec4(color, spot ? 24.0f : 12.0f);
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
    destroy_program(meshlet_program_);
    destroy_program(meshlet_shadow_program_);
    destroy_program(hiz_program_);
    destroy_program(reset_program_);
    destroy_program(draw_cull_program_);

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
        for (const string::gpu::resource_id b : stats_buffers_) allocator_.destroy_resource(b);
        for (const string::gpu::resource_id b : cull_cmd_buffers_) if (b) allocator_.destroy_resource(b);
        for (const string::gpu::resource_id b : cull_prepass_buffers_) if (b) allocator_.destroy_resource(b);
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

    descriptor_table_.unbind(white_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(white_image_);
    descriptor_table_.unbind(flat_normal_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(flat_normal_image_);
    for (const string::gpu::resource_id image : texture_images_)
    {
        descriptor_table_.unbind(image, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(image);
    }

    allocator_.destroy_resource(index_buffer_);
    allocator_.destroy_resource(vertex_buffer_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
{
    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);
    camera_.update(input_map_, delta_time, aspect);

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
        scene.ambient_ground = sky_ground_;
        for (uint32_t c = 0; c < settings_.cascade_count; ++c)
        {
            scene.cascade_view_proj[c] = cascade_view_proj_[c];
            scene.cascade_split[c] = glm::vec4(cascade_split_[c], 0, 0, 0);
            scene.cascade_texel[c] = glm::vec4(cascade_world_texel_[c], 0, 0, 0);
            scene.cascade_slot[c] = glm::uvec4(shadow_slots_[current_frame][c], 0, 0, 0);
        }
        scene.cascade_count = settings_.cascade_count;
        scene.shadow_texel = 1.0f / static_cast<float>(settings_.shadow_resolution);
        scene.shadow_bias = settings_.shadow_bias;
        scene.shadow_normal_offset_scale = settings_.shadow_normal_offset_scale;
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
        scene.debug_flags = froxel_heatmap_ ? 1u : 0u;
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
        }

        // Periodic culling-stats log (the numbers the UI overlay also shows). Sampled a few times so
        // the smoke run captures meaningful frustum/cone/HiZ reductions without spamming.
        if (stream_frame_ == 30 || stream_frame_ == 120 || stream_frame_ == 600)
        {
            const GpuMeshStats& s = stats_latest_;
            STRING_LOG_INFO("[mesh-cull] frame {}: meshlets {} -> frustum {} -> cone {} -> hiz {} "
                            "(LOD draws {}/{}/{}/{}){}",
                            stream_frame_, s.meshlets_total, s.after_frustum, s.after_cone, s.after_hiz,
                            s.draws_per_lod[0], s.draws_per_lod[1], s.draws_per_lod[2], s.draws_per_lod[3],
                            crowd_enabled_ ? " [crowd]" : "");
        }
    }

    ++stream_frame_;
}

bool GeometryPass::record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (draw_count_ == 0)
    {
        return false;
    }

    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

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

        // Brief 03b: build the GPU work lists for this frame. The camera list (selected LOD, writes
        // the LOD histogram) feeds the main pass + all 3 shadow cascades; the prepass list (LOD0)
        // feeds the HiZ depth prepass. Both are consumed later this frame (record_compute + record).
        if (draw_cull_program_ && active_draw_count_ > 0)
        {
            record_draw_cull(command_buffer, current_frame, /*prepass*/ false);
            if (hiz_enabled_ && stream_frame_ > 1)
                record_draw_cull(command_buffer, current_frame, /*prepass*/ true);
        }

        // Build the HiZ pyramid. Source: a single-sample depth-only CAMERA prepass of the resident,
        // frustum+cone-visible meshlets (the renderer's shared depth is 4x MSAA — unsamplable by a
        // plain Sampler2D — so the meshlet path renders its own 1x depth here). Then a MIN reduce
        // (reverse-Z: min = farthest) into the R32F pyramid. Skipped the first frames (no geometry
        // resident yet -> pyramid stays cleared -> HiZ passes everything; correct).
        if (hiz_enabled_ && hiz_program_ && meshlet_shadow_program_ && stream_frame_ > 1)
        {
            const HizPyramid& hz = hiz_[current_frame];
            const string::gpu::allocated_image& depth = allocator_.get_image(hz.depth);

            // --- 1x depth-only camera prepass (reuses the depth-only meshlet shadow pipeline, fed
            // the camera view-proj instead of a light view-proj). Frustum-culled, all resident draws.
            vku::transition_image(command_buffer, {
                .image = depth.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .new_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
                .dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            });
            const VkRenderingAttachmentInfo pre_depth = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = depth.view,
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = { .depthStencil = { 0.0f, 0 } },
            };
            const VkRenderingInfo pre_info = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = { { 0, 0 }, { screen_size.width, screen_size.height } },
                .layerCount = 1, .colorAttachmentCount = 0, .pDepthAttachment = &pre_depth,
            };
            vkCmdBeginRendering(command_buffer, &pre_info);
            const VkViewport pvp = { 0.0f, 0.0f, (float)screen_size.width, (float)screen_size.height, 0.0f, 1.0f };
            const VkRect2D psc = { { 0, 0 }, { screen_size.width, screen_size.height } };
            vkCmdSetViewport(command_buffer, 0, 1, &pvp);
            vkCmdSetScissor(command_buffer, 0, 1, &psc);
            const string::gpu::pipeline& pre = meshlet_shadow_program_->current();
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pre.pipeline);
            VkDescriptorSet preset = descriptor_table_.get_set();
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pre.pipeline_layout,
                                    0, 1, &preset, 0, nullptr);
            // Prepass draws the LOD0 work list (brief 03b): forced-LOD0 so the pyramid never records a
            // coarse LOD's receded/holed depth as an occluder the main pass then tests bounds against.
            MeshletShadowPush pp{};
            pp.light_view_proj = mesh_cull_frozen_ ? mesh_frozen_view_proj_ : camera_.view_proj();
            pp.vertices = allocator_.get_buffer(vertex_buffer_).device_address;
            pp.meshlets = allocator_.get_buffer(meshlet_buffer_).device_address;
            pp.mverts = allocator_.get_buffer(meshlet_vertices_).device_address;
            pp.mtris = allocator_.get_buffer(meshlet_triangles_).device_address;
            pp.draws = allocator_.get_buffer(draw_info_buffer_).device_address;
            const string::gpu::resource_id pre_list = cull_prepass_buffers_[current_frame];
            const VkBuffer pre_buf = allocator_.get_buffer(pre_list).buffer;
            pp.records = allocator_.get_buffer(pre_list).device_address + cull_records_offset_;
            vkCmdPushConstants(command_buffer, pre.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshletShadowPush), &pp);
            vkCmdDrawMeshTasksIndirectCountEXT(command_buffer, pre_buf, 0, pre_buf, cull_count_offset_,
                                               cull_max_draws_, sizeof(uint32_t) * 3);
            vkCmdEndRendering(command_buffer);

            // The prepass depth -> SHADER_READ for the mip-0 reduce.
            vku::transition_image(command_buffer, {
                .image = depth.image, .old_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            });
            // Transition our pyramid to GENERAL for storage writes.
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
                }
                else
                {
                    hpush.src_slot = hz.sample_slot;
                    hpush.src_size = glm::uvec2(std::max(1u, hz.size.x >> (m - 1)), std::max(1u, hz.size.y >> (m - 1)));
                    hpush.copy_depth = 0;
                }
                hpush.dst_slot = hz.mip_storage_slots[m];
                hpush.dst_size = glm::uvec2(dw, dh);
                vkCmdPushConstants(command_buffer, hp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(HizPush), &hpush);
                vkCmdDispatch(command_buffer, (dw + 7) / 8, (dh + 7) / 8, 1);
                // Barrier: this mip's writes visible to the next mip's sample.
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
            // Move the whole pyramid to SHADER_READ for the task shader's SampleLevel in record().
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
        }
    }

    // --- Froxel light binning: one thread per froxel bins the local lights into per-froxel index
    // lists that the lit fragment shader reads. Independent of the meshlet cull dispatch (different
    // buffers), so no barrier between them; the renderer's compute->draw barrier covers the read.
    if (froxel_program_ && froxel_count_ > 0 && current_frame < froxel_buffers_.size()
        && froxel_buffers_[current_frame] != 0)
    {
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

    // --- Cascaded shadow maps: render scene depth from the sun into each cascade's image, then
    // transition each to SHADER_READ for the lit pass. Draws the resident set via the meshlet shadow
    // path (off-screen casters included — cascades read the resident-only work list, NOT camera-culled).
    if (!shadow_images_.empty() && meshlet_shadow_program_ && draw_info_mapped_)
    {
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
            const string::gpu::resource_id cam_list = cull_cmd_buffers_[current_frame];
            const VkBuffer cam_buf = allocator_.get_buffer(cam_list).buffer;
            mspush.records = allocator_.get_buffer(cam_list).device_address + cull_records_offset_;
            vkCmdPushConstants(command_buffer, msh.pipeline_layout, VK_SHADER_STAGE_ALL,
                               0, sizeof(MeshletShadowPush), &mspush);
            // Cascades read the RESIDENT-ONLY command array (shadow_cmds): the camera-frustum
            // draw cull must not remove off-screen shadow CASTERS. Records are shared.
            vkCmdDrawMeshTasksIndirectCountEXT(command_buffer, cam_buf, cull_shadow_cmds_offset_,
                                               cam_buf, cull_count_offset_,
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
        const glm::mat4 vp = camera_.view_proj();
        const SkyPush sky_push{
            .inv_view_proj = glm::inverse(vp),
            .camera_pos = camera_.position(),
            .sun_dir = sun_dir_,
            .sky_zenith = sky_zenith_,
            .sky_ground = sky_ground_,
            .sun_color = sun_color_,
        };
        const string::gpu::pipeline& sky_p = sky_program_->current();
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_p.pipeline);
        vkCmdPushConstants(command_buffer, sky_p.pipeline_layout,
                           sky_p.push_constants.stageFlags, 0, sizeof(SkyPush), &sky_push);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
    }

    // --- Brief 03: task/mesh meshlet draw path (the scene geometry front-end) -----------------
    if (meshlet_program_ && draw_info_mapped_)
    {
        const string::gpu::pipeline& mp = meshlet_program_->current();
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline);
        VkDescriptorSet mset = descriptor_table_.get_set();
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                mp.pipeline_layout, 0, 1, &mset, 0, nullptr);
        // Single main pass: every resident meshlet is frustum+cone culled in the task shader and
        // tested against THIS frame's HiZ pyramid (built from a complete depth prepass). No two-pass
        // bitfield — the prepass is complete, so one conservative test per meshlet is correct and
        // deterministic (no dependence on last frame's draw set). HiZ off -> hiz_mips==0 -> all pass.
        record_meshlet_draws(command_buffer, mp, current_frame);
    }
}

}  // namespace sandbox
