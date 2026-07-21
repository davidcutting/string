#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Is the world-space AABB inside the frustum? Mirrors cull.comp's aabb_outside_frustum (Gribb-
// Hartmann planes from the view-projection, ZERO_TO_ONE clip) on the CPU, for geometry-residency
// feedback — we want a draw's geometry as soon as it's potentially visible.
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

GeometryPass::GeometryPass(PassContext& context, const std::filesystem::path& model_path)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, frames_in_flight_(context.frames_in_flight)
, residency_(kTextureBudget, kTextureStreamPerFrame)
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
    // Two per-draw buffers, built once: DrawData (transform + material + texture slot) that the
    // vertex shader indexes by gl_InstanceIndex, and CullDraw (world AABB + command fields) that
    // the cull compute shader reads to emit the frame's indirect commands (see record_compute).
    draw_count_ = static_cast<uint32_t>(draws_.size());
    if (draw_count_ > 0)
    {
        std::vector<DrawData> draw_data(draws_.size());
        std::vector<CullDraw> cull_data(draws_.size());
        for (std::size_t i = 0; i < draws_.size(); ++i)
        {
            const GltfDraw& draw = draws_[i];
            DrawData& data = draw_data[i];
            data.model = draw.transform;
            if (draw.material >= 0)
            {
                const GltfMaterial& material = materials_[draw.material];
                data.base_color = material.base_color_factor;
                data.base_slot = material.base_color_texture >= 0
                    ? texture_slots_[material.base_color_texture]
                    : white_slot_;
                data.normal_slot = material.normal_texture >= 0
                    ? texture_slots_[material.normal_texture]
                    : flat_normal_slot_;
                data.mr_slot = material.metallic_roughness_texture >= 0
                    ? texture_slots_[material.metallic_roughness_texture]
                    : white_slot_;
                data.metallic = material.metallic_factor;
                data.roughness = material.roughness_factor;
            }
            else
            {
                data.base_color = glm::vec4(1.0f);
                data.base_slot = white_slot_;
                data.normal_slot = flat_normal_slot_;
                data.mr_slot = white_slot_;
                data.metallic = 1.0f;
                data.roughness = 1.0f;
            }

            cull_data[i] = CullDraw{
                .aabb_min = glm::vec4(draw.aabb_min, 1.0f),
                .aabb_max = glm::vec4(draw.aabb_max, 1.0f),
                // index_count starts at 0 (not resident) — the geometry streamer writes the real
                // count into the (host-visible) cull buffer once the draw's geometry is uploaded,
                // so cull.comp only emits a non-empty draw for resident geometry.
                .index_count = 0,
                .first_index = draw.index_offset,
                .vertex_offset = 0,
                .draw_id = static_cast<uint32_t>(i),   // -> firstInstance -> gl_InstanceIndex
            };
        }

        const VkDeviceSize draw_data_size = sizeof(DrawData) * draw_data.size();
        draw_data_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
            .size = draw_data_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(draw_data.data(), draw_data_size, draw_data_buffer_);
        descriptor_table_.bind(draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);
        draw_data_slot_ = descriptor_table_.get_binding_slot(
            draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);

        // Cull input (device-addressed so the compute shader reaches it by pointer). Host-visible
        // and persistently mapped: the geometry streamer flips each draw's index_count between 0 and
        // its real value directly (per-frame residency), so it must be CPU-writable. Desktop
        // host-visible memory is coherent, and vkQueueSubmit makes prior host writes visible, so the
        // per-frame writes in update() are seen by that frame's cull.comp. No TRANSFER_DST (no copy).
        const VkDeviceSize cull_size = sizeof(CullDraw) * cull_data.size();
        cull_draw_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
            .size = cull_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        });
        cull_draw_mapped_ = static_cast<CullDraw*>(
            allocator_.get_buffer(cull_draw_buffer_).allocation_info.pMappedData);
        std::copy(cull_data.begin(), cull_data.end(), cull_draw_mapped_);

        // Cull output: the compute shader fills one command per draw each frame, drawn via
        // vkCmdDrawIndexedIndirect. One per frame-in-flight so a new frame's compute doesn't
        // clobber a still-in-flight frame's draw.
        culled_indirect_buffers_.resize(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            culled_indirect_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
                .size = sizeof(VkDrawIndexedIndirectCommand) * draw_count_,
                .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                .allocation_flags = {},
            });
        }

        // Cull compute pipeline: push-constant only (buffers reached by device address).
        const VkPushConstantRange cull_push_range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(CullPush),
        };
        cull_pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
            .set_push_constant_ranges({ cull_push_range })
            .build(device_);
        cull_pipeline_.pipeline = string::gpu::pipeline_builder(device_, string::gpu::pipeline_type::COMPUTE)
            .add_compute_shader(context.resources_path / "shaders/cull.comp.spv")
            .build_compute_pipeline(cull_pipeline_.pipeline_layout);
        cull_pipeline_.pipeline_type = string::gpu::pipeline_type::COMPUTE;

        // Geometry streaming: hand the CPU geometry to the streamer (suballocates + uploads per-draw
        // ranges on demand, frees on eviction) and register every draw with the geometry residency
        // manager. The manager's byte budget matches the heap capacity so it evicts (reclaims) once
        // the heaps are full. On residency the streamer writes the draw's indirect-command fields
        // into the mapped cull buffer, revealing it; on eviction it zeroes them.
        geometry_streamer_ = std::make_unique<GeometryStreamer>(
            allocator_, context.transfer, vertex_buffer_, index_buffer_,
            std::move(geometry.vertices), std::move(geometry.indices),
            vertex_capacity, index_capacity, frames_in_flight_);
        geometry_streamer_->set_draws(draws_);
        geometry_streamer_->set_residency_callback(
            [this](std::uint32_t draw, std::uint32_t vertex_offset, std::uint32_t first_index,
                   std::uint32_t index_count) {
                cull_draw_mapped_[draw].vertex_offset = vertex_offset;
                cull_draw_mapped_[draw].first_index = first_index;
                cull_draw_mapped_[draw].index_count = index_count;
            });

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
    camera_.frame_bounds(aabb_min, aabb_max);

    // Declared graph usages: the shared buffers + the render targets. The uploaded textures are
    // static SHADER_READ_ONLY inputs (transitioned once by the transfer batch), so they're not
    // graph-tracked and don't need declaring here.
    usages = {
        { vertex_buffer_,       Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { index_buffer_,        Access::IndexRead,   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT },
        { draw_data_buffer_,    Access::StorageRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        { context.depth_target, Access::DepthWrite,  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT },
    };
    // The indirect buffer's DRAW_INDIRECT read is covered by the transfer batch's global barrier
    // (like the static textures); once buffer state is graph-tracked it can be declared here too.

    // view_proj + drawdata_slot are read only by the vertex stage now (the fragment stage reads
    // the material tint/slot from varyings).
    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(GeometryPush),
    };

    pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
        .set_descriptor_set_layout({ descriptor_table_.get_layout() })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    // No vertex-input state: the shader pulls vertices from a device-address SSBO by gl_VertexIndex
    // (the builder defaults to an empty VkPipelineVertexInputStateCreateInfo).
    pipeline_.pipeline = string::gpu::pipeline_builder(device_)
        .add_vertex_shader(context.resources_path / "shaders/3d_shader.vert.spv")
        .add_fragment_shader(context.resources_path / "shaders/3d_shader.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        // Back-face cull with a CCW front face: glTF winds front faces CCW, and this pipeline's
        // GLM projection + Y-flip keep that winding front-facing in the framebuffer. CLOCKWISE
        // here culls the wrong faces.
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .set_multisampling(context.sample_count)
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_.pipeline_layout);
    pipeline_.pipeline_type = string::gpu::pipeline_type::GRAPHICS;

    // --- Directional shadow map ---------------------------------------------------------------
    if (draw_count_ > 0)
    {
        compute_light_matrix();

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

        // One shadow depth image per frame in flight so frame N+1's render doesn't race N's sample.
        shadow_images_.resize(frames_in_flight_);
        shadow_slots_.resize(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            shadow_images_[f] = allocator_.create_resource(string::gpu::image_info{
                .extent = { kShadowResolution, kShadowResolution, 1 },
                .format = VK_FORMAT_D32_SFLOAT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
                .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                .allocation_flags = {},
            });
            descriptor_table_.bind(shadow_images_[f], string::gpu::descriptor_type::TEXTURE);
            shadow_slots_[f] = descriptor_table_.get_binding_slot(shadow_images_[f], string::gpu::descriptor_type::TEXTURE);
            descriptor_table_.update_texture(shadow_slots_[f], allocator_.get_image(shadow_images_[f]).view, shadow_sampler_);
        }

        // Static all-visible indirect: every draw is rendered into the shadow map (no camera cull).
        std::vector<VkDrawIndexedIndirectCommand> shadow_cmds(draws_.size());
        for (std::size_t i = 0; i < draws_.size(); ++i)
        {
            shadow_cmds[i] = VkDrawIndexedIndirectCommand{
                .indexCount = draws_[i].index_count,
                .instanceCount = 1,
                .firstIndex = draws_[i].index_offset,
                .vertexOffset = 0,
                .firstInstance = static_cast<uint32_t>(i),   // -> gl_InstanceIndex -> DrawData
            };
        }
        const VkDeviceSize shadow_indirect_size = sizeof(VkDrawIndexedIndirectCommand) * shadow_cmds.size();
        shadow_indirect_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
            .size = shadow_indirect_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(shadow_cmds.data(), shadow_indirect_size, shadow_indirect_buffer_);

        // Depth-only shadow pipeline (vertex-only, reverse-Z to match the engine). Cull nothing so
        // single-sided geometry still casts (avoids light leaks); rely on depth bias for acne.
        const VkPushConstantRange shadow_push_range = {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(ShadowPush),
        };
        shadow_pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({ descriptor_table_.get_layout() })
            .set_push_constant_ranges({ shadow_push_range })
            .build(device_);
        shadow_pipeline_.pipeline = string::gpu::pipeline_builder(device_)
            .add_vertex_shader(context.resources_path / "shaders/shadow.vert.spv")
            .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .set_tessellation()
            .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_multisampling()
            .enable_depth_stencil()
            .depth_only()
            .build_graphics_pipeline(shadow_pipeline_.pipeline_layout);
        shadow_pipeline_.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
    }
}

void GeometryPass::compute_light_matrix()
{
    // World-space scene AABB (union of every draw's box).
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    for (const GltfDraw& d : draws_)
    {
        mn = glm::min(mn, d.aabb_min);
        mx = glm::max(mx, d.aabb_max);
    }
    const glm::vec3 center = (mn + mx) * 0.5f;
    const float radius = glm::length(mx - mn) * 0.5f;

    const glm::vec3 L = glm::normalize(sun_dir_);   // direction TO the light
    const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 eye = center + L * radius;       // place the light just outside the scene sphere
    const glm::mat4 view = glm::lookAt(eye, center, up);

    // Fit an orthographic box to the AABB's 8 corners in light space.
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        const glm::vec3 ls = glm::vec3(view * glm::vec4(corner, 1.0f));
        lo = glm::min(lo, ls);
        hi = glm::max(hi, ls);
    }
    // RH view looks down -z (nearer = larger z); glm::ortho near/far are positive distances in
    // front of the eye, so near = -hi.z, far = -lo.z. Pull near toward the light so occluders
    // between it and the box still cast.
    const float near_plane = -hi.z - radius * 0.5f;
    const float far_plane = -lo.z;
    // World units per shadow texel (light space is a pure rotation, so it preserves world scale) —
    // used to size the normal-offset bias in the fragment shader.
    shadow_world_texel_ = std::max(hi.x - lo.x, hi.y - lo.y) / static_cast<float>(kShadowResolution);
    glm::mat4 proj = glm::ortho(lo.x, hi.x, lo.y, hi.y, near_plane, far_plane);  // ZERO_TO_ONE depth
    proj[1][1] *= -1.0f;  // Vulkan Y-flip

    // Reverse-Z (near->1, far->0) to match the engine (GREATER_OR_EQUAL compare, clear 0).
    glm::mat4 reverse_z(1.0f);
    reverse_z[2][2] = -1.0f;
    reverse_z[3][2] = 1.0f;
    proj = reverse_z * proj;

    light_view_proj_ = proj * view;
}

GeometryPass::~GeometryPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);

    if (!shadow_images_.empty())
    {
        vkDestroyPipeline(device_.get_device(), shadow_pipeline_.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), shadow_pipeline_.pipeline_layout, nullptr);
        vkDestroySampler(device_.get_device(), shadow_sampler_, nullptr);
        for (const string::gpu::resource_id image : shadow_images_)
        {
            descriptor_table_.unbind(image, string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(image);
        }
        allocator_.destroy_resource(shadow_indirect_buffer_);
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

    if (draw_count_ > 0)
    {
        vkDestroyPipeline(device_.get_device(), cull_pipeline_.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), cull_pipeline_.pipeline_layout, nullptr);

        descriptor_table_.unbind(draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(draw_data_buffer_);
        allocator_.destroy_resource(cull_draw_buffer_);
        for (const string::gpu::resource_id buffer : culled_indirect_buffers_)
            allocator_.destroy_resource(buffer);
    }

    allocator_.destroy_resource(index_buffer_);
    allocator_.destroy_resource(vertex_buffer_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
{
    (void)current_frame;

    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);
    camera_.update(input_map_, delta_time, aspect);

    // Toggle the debug frozen culling frustum. On freeze, snapshot the current view-projection;
    // the camera keeps moving but the cull test stays against the snapshot, so culled geometry
    // becomes visible as it leaves the frozen view.
    if (input_map_.pressed("freeze_culling"))
    {
        cull_frozen_ = !cull_frozen_;
        if (cull_frozen_)
        {
            frozen_cull_view_proj_ = camera_.view_proj();
        }
        STRING_LOG_INFO("Cull frustum {}", cull_frozen_ ? "FROZEN (debug)" : "live");
    }
    if (input_map_.pressed("toggle_culling"))
    {
        cull_enabled_ = !cull_enabled_;
        STRING_LOG_INFO("GPU frustum culling {}", cull_enabled_ ? "ON" : "OFF (debug)");
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
    ++stream_frame_;
}

bool GeometryPass::record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (draw_count_ == 0)
    {
        return false;
    }

    VkCommandBuffer& command_buffer = recorder.get_command_buffer();
    const string::gpu::resource_id indirect_id = culled_indirect_buffers_[current_frame];

    // Dispatch one thread per draw: each writes its indirect command (instanceCount 0 if the
    // draw's AABB is outside the camera frustum). The compute -> draw-indirect barrier is emitted
    // by the renderer after the compute prepass.
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_.pipeline);
    const CullPush push{
        .view_proj = cull_frozen_ ? frozen_cull_view_proj_ : camera_.view_proj(),
        .cull_in = allocator_.get_buffer(cull_draw_buffer_).device_address,
        .indirect_out = allocator_.get_buffer(indirect_id).device_address,
        .draw_count = draw_count_,
        .cull_enabled = cull_enabled_ ? 1u : 0u,
    };
    vkCmdPushConstants(command_buffer, cull_pipeline_.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush), &push);
    vkCmdDispatch(command_buffer, (draw_count_ + 63) / 64, 1, 1);

    // --- Shadow map: render scene depth from the sun into this frame's shadow image. Self-contained
    // dynamic rendering (outside the main group), at shadow resolution, then transitioned to
    // SHADER_READ so the lit fragment shader can sample it. The all-visible indirect is static, so
    // this needs no barrier against the cull dispatch above.
    if (!shadow_images_.empty())
    {
        const string::gpu::allocated_image& shadow = allocator_.get_image(shadow_images_[current_frame]);

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
            .renderArea = { { 0, 0 }, { kShadowResolution, kShadowResolution } },
            .layerCount = 1,
            .colorAttachmentCount = 0,
            .pColorAttachments = nullptr,
            .pDepthAttachment = &depth_att,
        };
        vkCmdBeginRendering(command_buffer, &shadow_render);

        const VkViewport vp = { 0.0f, 0.0f, static_cast<float>(kShadowResolution),
                                static_cast<float>(kShadowResolution), 0.0f, 1.0f };
        const VkRect2D sc = { { 0, 0 }, { kShadowResolution, kShadowResolution } };
        vkCmdSetViewport(command_buffer, 0, 1, &vp);
        vkCmdSetScissor(command_buffer, 0, 1, &sc);

        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_.pipeline);
        VkDescriptorSet set = descriptor_table_.get_set();
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            shadow_pipeline_.pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdBindIndexBuffer(command_buffer, allocator_.get_buffer(index_buffer_).buffer, 0, VK_INDEX_TYPE_UINT32);
        const ShadowPush spush = {
            .light_view_proj = light_view_proj_,
            .vertex_address = allocator_.get_buffer(vertex_buffer_).device_address,
            .drawdata_slot = draw_data_slot_,
        };
        vkCmdPushConstants(command_buffer, shadow_pipeline_.pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPush), &spush);
        vkCmdDrawIndexedIndirect(command_buffer, allocator_.get_buffer(shadow_indirect_buffer_).buffer,
            0, draw_count_, sizeof(VkDrawIndexedIndirectCommand));

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

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);

    const auto& vertex_buffer = allocator_.get_buffer(vertex_buffer_);
    const auto& index_buffer = allocator_.get_buffer(index_buffer_);

    // No vertex binding — the shader pulls vertices from vertex_buffer's device address (below).
    // Indices are still fetched fixed-function by the indexed indirect draw.
    vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    VkDescriptorSet set = descriptor_table_.get_set();
    vkCmdBindDescriptorSets(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_.pipeline_layout, 0, 1, &set, 0, nullptr);

    // Per-frame camera + vertex SSBO address + draw-data slot (vertex stage), plus the forward
    // lighting constants (fragment stage): a single fixed directional sun and a hemispheric ambient.
    // Sun points down from front-left; warm sun, cool sky, warm bounce.
    const GeometryPush push{
        .view_proj = camera_.view_proj(),
        .vertex_address = vertex_buffer.device_address,
        .drawdata_slot = draw_data_slot_,
        .camera_pos = camera_.position(),
        .sun_dir = sun_dir_,
        .sun_intensity = 3.0f,
        .sun_color = glm::vec3(1.0f, 0.96f, 0.9f),
        .ambient_sky = glm::vec3(0.10f, 0.13f, 0.20f),
        .ambient_ground = glm::vec3(0.10f, 0.08f, 0.06f),
        .light_view_proj = light_view_proj_,
        .shadow_slot = shadow_slots_.empty() ? 0u : shadow_slots_[current_frame],
        .shadow_texel = 1.0f / static_cast<float>(kShadowResolution),
        .shadow_bias = 0.0006f,
        .shadow_normal_offset = shadow_world_texel_ * 2.5f,
    };
    vkCmdPushConstants(
        command_buffer, pipeline_.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GeometryPush), &push);

    // Draw every command the cull compute shader emitted this frame; culled draws carry
    // instanceCount 0 so the GPU skips them. Each selects its DrawData via firstInstance ->
    // gl_InstanceIndex.
    const auto& culled_indirect = allocator_.get_buffer(culled_indirect_buffers_[current_frame]);
    vkCmdDrawIndexedIndirect(command_buffer, culled_indirect.buffer, 0, draw_count_,
                             sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace sandbox
