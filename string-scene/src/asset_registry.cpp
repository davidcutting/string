#include <string/scene/asset_registry.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <optional>

#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>

#include <string/asset/manifest.hpp>

// The single stb_image implementation for the engine lives here (the registry decodes the
// non-cooked texture fallback path).
#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

namespace string::assets
{

using ::string::asset::CookedDraw;
using ::string::asset::CookedMaterial;
using ::string::asset::CookedScene;
using ::string::asset::CookedSkin;
using ::string::asset::CookedTexture;
using ::string::asset::GpuMeshlet;
using ::string::asset::kMaxLods;

namespace
{

// The coarse-tail detail (resident mip levels from the coarsest) whose finest level is ~floor_px
// wide — the LOD floor every streamed texture is pinned to. detail == levels means all mips.
uint32_t coarse_detail_for(uint32_t levels, uint32_t base_extent, uint32_t floor_px)
{
    uint32_t base_mip = base_extent <= floor_px
        ? 0u
        : static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(base_extent) / floor_px)));
    base_mip = std::min(base_mip, levels - 1);
    return levels - base_mip;
}

// A 1x1 opaque white stand-in. Multiplying by white is the identity for every slot that takes a
// colour map, so a texture we could not decode costs that surface its detail and nothing else.
decoded_image white_fallback(bool srgb)
{
    auto* pixels = static_cast<stbi_uc*>(STBI_MALLOC(4));
    pixels[0] = pixels[1] = pixels[2] = pixels[3] = 0xFF;
    decoded_image decoded;
    decoded.pixels = { pixels, [](void* p) { stbi_image_free(static_cast<stbi_uc*>(p)); } };
    decoded.width = 1;
    decoded.height = 1;
    decoded.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return decoded;
}

// Decode one texture source to RGBA8. Pure CPU work — safe to run on a job thread. NEVER throws: a
// texture is content, and bad content must not be able to kill the engine. (Embedded glTF images
// are extracted to sibling files at cook, so the texture table is paths-only; an empty path means
// the source was lost at bake.)
decoded_image decode_texture(const texture_desc& source)
{
    if (source.file.empty())
    {
        STRING_LOG_WARN("[load] texture has no source (embedded image lost at bake?); using white");
        return white_fallback(source.srgb);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(source.file.string().c_str(), &width, &height, &channels,
                                STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0)
    {
        if (pixels) stbi_image_free(pixels);
        STRING_LOG_WARN("[load] failed to decode texture {}: {}; using white",
                        source.file.string(),
                        stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return white_fallback(source.srgb);
    }

    decoded_image decoded;
    decoded.pixels = { pixels, [](void* p) { stbi_image_free(static_cast<stbi_uc*>(p)); } };
    decoded.width = width;
    decoded.height = height;
    decoded.format = source.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return decoded;
}

// The cooked `.ktx2` sibling of an external texture source, if one exists on disk. This lets
// cooking be incremental: a texture with a sibling loads via the fast KTX path, the rest fall
// back to stb.
std::optional<std::filesystem::path> ktx_sibling(const texture_desc& source)
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

}  // namespace

registry::registry(engine_context& ctx, const registry_config& config, cook_provider* cook)
: ctx_(&ctx)
, config_(config)
, cook_(cook)
, pool_(std::make_unique<texture_pool>(ctx.allocator, ctx.descriptor_table, ctx.transfer))
, texture_residency_(std::make_unique<::string::gpu::residency_manager>(
      config.texture_budget_bytes, config.texture_stream_bytes_per_frame))
{
}

registry::registry(const registry_config& config, cook_provider* cook)
: config_(config)
, cook_(cook)
{
}

registry::~registry()
{
    // The heap buffers are the registry's own creations; the pool destroys its images itself.
    if (ctx_ == nullptr) return;
    for (const ::string::gpu::resource_id id :
         { vertex_buffer_, meshlet_buffer_, meshlet_vertices_buffer_, meshlet_triangles_buffer_,
           skin_buffer_ })
    {
        if (id != 0) ctx_->allocator.destroy_resource(id);
    }
}

asset_id registry::load(const std::filesystem::path& source, load_error* err)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto set_err = [err](load_error e) { if (err != nullptr) *err = e; };
    set_err(load_error::none);

    // Interning: the registry key is the source path's generic string.
    const std::string name = source.generic_string();
    if (const asset_id existing = find(name); existing.valid()) return existing;

    if (!std::filesystem::exists(source))
    {
        set_err(load_error::not_found);
        STRING_LOG_WARN("[assets] source not found: {}", name);
        return {};
    }

    const std::filesystem::path cooked_path =
        ::string::asset::cooked_path_for(source, config_.chunk_budget);

    CookedScene scene;
    bool ok = false;
    if (!::string::asset::needs_recook(source, cooked_path, config_.chunk_budget))
    {
        ok = ::string::asset::read_cooked_file(cooked_path, scene);
        if (ok) ++stats_.cooked_hits;
        else
            STRING_LOG_WARN("[load] cooked file unreadable, re-cooking: {}", cooked_path.string());
    }
    if (!ok)
    {
        // Missing / stale / unreadable. With a provider (dev/editor build) cook in-process;
        // without one (shipped client) this is a clean, reportable failure.
        if (cook_ == nullptr)
        {
            set_err(load_error::not_cooked);
            STRING_LOG_WARN("[assets] no fresh cooked file for {} and no cook provider "
                            "-- run `nix run .#cook -- {}` to precook",
                            name, name);
            return {};
        }
        STRING_LOG_WARN("[load] cooking in-process (missing/stale cooked scene): {} "
                        "-- run `nix run .#cook -- {}` to precook",
                        name, name);
        if (!cook_->cook_geometry(source, config_.chunk_budget, scene))
        {
            set_err(load_error::malformed);
            STRING_LOG_WARN("[assets] cook failed for {}", name);
            return {};
        }
        ++stats_.cooked_misses;
    }

    const asset_id id = insert(std::move(scene), name, source.parent_path(),
                               ::string::asset::anim_path_for(source));
    stats_.load_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return id;
}

asset_id registry::load_baked(CookedScene&& scene, std::string_view name,
                              const std::filesystem::path& base_dir)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (const asset_id existing = find(name); existing.valid()) return existing;
    const asset_id id = insert(std::move(scene), name, base_dir, {});
    stats_.load_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return id;
}

const asset* registry::get(asset_id id) const
{
    if (!id.valid() || id.index >= assets_.size()) return nullptr;
    return &assets_[id.index];
}

asset_id registry::find(std::string_view name) const
{
    for (uint32_t i = 0; i < assets_.size(); ++i)
        if (assets_[i].name() == name) return asset_id{ .index = i, .generation = 0 };
    return {};
}

// Create the GPU side of a texture range just merged into the table: cooked KTX2 siblings register
// with the pool's streaming path (BC7 image, coarse-tail-first residency), the rest decode via stb
// on a worker pool and upload whole. Slot order = table order, per file.
void registry::register_textures(uint32_t first_texture, uint32_t count,
                                 std::span<const CookedTexture> /*cooked*/)
{
    if (count == 0 || ctx_ == nullptr) return;

    ::string::core::job_system decode_pool;
    std::vector<std::optional<std::filesystem::path>> ktx_paths(count);
    std::vector<std::future<decoded_image>> decode_jobs(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const texture_desc& t = textures_[first_texture + i];
        ktx_paths[i] = ktx_sibling(t);
        if (!ktx_paths[i])
        {
            const texture_desc* source = &t;
            decode_jobs[i] = decode_pool.enqueue([source]() { return decode_texture(*source); });
        }
    }

    texture_runtime_.resize(textures_.size());
    frame_desired_detail_.resize(textures_.size(), 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        texture_runtime& rt = texture_runtime_[first_texture + i];
        if (ktx_paths[i])
        {
            const texture_pool::Registered reg =
                pool_->add(*ktx_paths[i], textures_[first_texture + i].srgb);
            rt.image = reg.image;
            rt.slot = reg.slot;
            rt.levels = reg.max_detail;
            rt.base_extent = reg.base_extent;
            rt.coarse_detail =
                coarse_detail_for(reg.max_detail, reg.base_extent, config_.coarse_floor_pixels);
            texture_residency_->register_resource(reg.image, *pool_, reg.min_detail, reg.max_detail,
                                                 reg.min_detail);
            streamed_.push_back(reg.image);
        }
        else
        {
            const decoded_image decoded = decode_jobs[i].get();
            const texture_pool::Uploaded up = pool_->add_decoded(decoded);
            rt.image = up.image;
            rt.slot = up.slot;
            rt.levels = 0;   // whole-uploaded; not streamed
        }
    }
    STRING_LOG_INFO("[stream] {} of {} textures streamed (BC7, coarse tail first); rest stb",
                    streamed_.size(), textures_.size());
}

// Merge one cooked scene into the global tables — the ONE place the cooked format's file-local
// index spaces are rebased (was triplicated across scene_loader, the transparency test-content
// injection and the lookdev builder).
asset_id registry::insert(CookedScene&& scene, std::string_view name,
                          const std::filesystem::path& base_dir,
                          const std::filesystem::path& anim_pack)
{
    const uint32_t vertex_base = static_cast<uint32_t>(vertices_.size());
    const uint32_t meshlet_base = static_cast<uint32_t>(meshlets_.size());
    const uint32_t mvert_base = static_cast<uint32_t>(meshlet_vertices_.size());
    const uint32_t mtri_base = static_cast<uint32_t>(meshlet_triangles_.size());
    const uint32_t tex_base = static_cast<uint32_t>(textures_.size());
    const uint32_t material_base = static_cast<uint32_t>(materials_.size());
    const uint32_t skin_base = static_cast<uint32_t>(skins_.size());
    const uint32_t skin_vert_base = static_cast<uint32_t>(skin_vertices_.size());
    const uint32_t part_base = static_cast<uint32_t>(mesh_parts_.size());

    if (vertex_buffer_ != 0)
    {
        // The heaps were already sized and uploaded (declare() ran). Post-declare loading needs
        // heap suballocation-on-load, which arrives with cross-scene asset pinning; until then
        // this is a hard, clean error rather than silent corruption.
        STRING_LOG_WARN("[assets] load after declare() is not supported yet: {}", name);
        return {};
    }

    // Vertices: append (heap grows; meshlet-vertex remap entries rebase by vertex_base below).
    vertices_.insert(vertices_.end(), scene.vertices.begin(), scene.vertices.end());

    // Meshlet-vertex remap: file-local global vertex indices -> the merged heap.
    meshlet_vertices_.reserve(meshlet_vertices_.size() + scene.meshlet_vertices.size());
    for (uint32_t v : scene.meshlet_vertices) meshlet_vertices_.push_back(v + vertex_base);

    // Triangle words: local 8-bit indices, no rebasing (they index within a meshlet's vertex run).
    meshlet_triangles_.insert(meshlet_triangles_.end(), scene.meshlet_triangles.begin(),
                              scene.meshlet_triangles.end());

    // Meshlets: their vertex_offset/triangle_offset index the merged heaps.
    meshlets_.reserve(meshlets_.size() + scene.meshlets.size());
    for (GpuMeshlet m : scene.meshlets)
    {
        m.vertex_offset += mvert_base;
        m.triangle_offset += mtri_base;
        meshlets_.push_back(m);
    }

    // Textures: resolve relative paths against this file's directory. An empty cooked path means an
    // embedded/absent source; the empty file path degrades to the white fallback downstream.
    for (const CookedTexture& ct : scene.textures)
    {
        texture_desc t;
        t.srgb = ct.srgb != 0;
        if (ct.path[0] != '\0') t.file = base_dir / std::filesystem::path(std::string(ct.path));
        textures_.push_back(std::move(t));
    }

    // Materials: texture indices become registry-global handles.
    for (const CookedMaterial& cm : scene.materials)
    {
        const auto tex = [tex_base](int32_t idx) {
            return idx >= 0 ? texture_id{ static_cast<uint32_t>(idx) + tex_base } : texture_id{};
        };
        material m;
        m.base_color_factor = cm.base_color_factor;
        m.metallic_factor = cm.metallic_factor;
        m.roughness_factor = cm.roughness_factor;
        m.alpha_cutoff = cm.alpha_cutoff;
        m.base_color = tex(cm.base_color_texture);
        m.metallic_roughness = tex(cm.metallic_roughness_texture);
        m.normal = tex(cm.normal_texture);
        m.occlusion = tex(cm.occlusion_texture);
        m.alpha_mode = static_cast<::string::asset::CookedAlphaMode>(cm.alpha_mode);
        m.double_sided = cm.double_sided != 0;
        materials_.push_back(m);
    }

    // Skins (brief 23): the skin stream appends COMPACTLY (parallel to ITS file's vertices, so the
    // per-part rebase delta is uniform per file); per-skin windows rebase into the merged tables.
    if (!scene.skin_vertices.empty())
    {
        skin_vertices_.insert(skin_vertices_.end(), scene.skin_vertices.begin(),
                              scene.skin_vertices.end());
        const uint32_t ibm_base = static_cast<uint32_t>(inverse_bind_.size());
        const uint32_t remap_base = static_cast<uint32_t>(joint_remap_.size());
        for (const CookedSkin& cs : scene.skins)
        {
            skins_.push_back(skin_binding{
                .skeleton_hash = cs.skeleton_hash,
                .joint_count = cs.joint_count,
                .ibm_offset = cs.ibm_offset + ibm_base,
                .remap_offset = cs.remap_offset + remap_base,
                .anim_pack = anim_pack,
            });
        }
        inverse_bind_.insert(inverse_bind_.end(), scene.inverse_bind.begin(),
                             scene.inverse_bind.end());
        joint_remap_.insert(joint_remap_.end(), scene.joint_remap.begin(),
                            scene.joint_remap.end());
    }

    // Mesh parts: one record per cooked draw, in GLOBAL coordinates.
    glm::vec3 bounds_min(std::numeric_limits<float>::max());
    glm::vec3 bounds_max(std::numeric_limits<float>::lowest());
    for (const CookedDraw& cd : scene.draws)
    {
        mesh_part p;
        p.material = cd.material >= 0
                         ? material_id{ static_cast<uint32_t>(cd.material) + material_base }
                         : material_id{};
        p.transform = cd.transform;
        p.aabb_min = cd.aabb_min;
        p.aabb_max = cd.aabb_max;
        p.center = cd.center;
        p.radius = cd.radius;
        p.first_meshlet = cd.first_meshlet + meshlet_base;
        p.total_meshlets = cd.total_meshlets;
        p.lod_count = cd.lod_count;
        for (uint32_t l = 0; l < kMaxLods; ++l)
        {
            p.lods[l] = cd.lods[l];
            if (l < cd.lod_count) p.lods[l].meshlet_offset += meshlet_base;
        }
        p.vertex_offset = cd.vertex_count == 0 ? 0u : cd.vertex_offset + vertex_base;
        p.vertex_count = cd.vertex_count;

        const bool skinned = cd.skin_plus_one > 0 && !scene.skin_vertices.empty();
        p.skin = skinned ? skin_id{ skin_base + cd.skin_plus_one - 1 } : skin_id{};
        p.skin_delta = skinned ? static_cast<int32_t>(skin_vert_base)
                                     - static_cast<int32_t>(vertex_base)
                               : 0;

        bounds_min = glm::min(bounds_min, cd.aabb_min);
        bounds_max = glm::max(bounds_max, cd.aabb_max);
        mesh_parts_.push_back(p);
    }
    if (scene.draws.empty())
    {
        bounds_min = glm::vec3(0.0f);
        bounds_max = glm::vec3(0.0f);
    }

    // The texture GPU side (images + bindless slots), in table order.
    register_textures(tex_base, static_cast<uint32_t>(scene.textures.size()), scene.textures);

    asset a;
    a.name_ = std::string(name);
    a.content_hash_ = scene.source_content_hash;
    a.first_mesh_ = part_base;
    a.mesh_count_ = static_cast<uint32_t>(scene.draws.size());
    a.first_material_ = material_base;
    a.material_count_ = static_cast<uint32_t>(scene.materials.size());
    a.first_texture_ = tex_base;
    a.texture_count_ = static_cast<uint32_t>(scene.textures.size());
    a.first_skin_ = skin_base;
    a.skin_count_ = static_cast<uint32_t>(scene.skins.size());
    a.bounds_min_ = bounds_min;
    a.bounds_max_ = bounds_max;
    assets_.push_back(std::move(a));

    ++stats_.files;
    ++view_.revision;
    return asset_id{ .index = static_cast<uint32_t>(assets_.size()) - 1, .generation = 0 };
}

void registry::declare(frame_graph& fg)
{
    view_.meshlet_capacity = total_meshlets();

    auto make_device_buffer = [&](const char* name, const void* data, VkDeviceSize size,
                                  VkBufferUsageFlags extra) {
        ::string::gpu::resource_id id =
            ctx_->allocator.create_resource(::string::gpu::buffer_info{
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
                .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                .allocation_flags = {},
            });
        if (data != nullptr) ctx_->transfer.upload_buffer(data, size, id);
        (void)name;
        return id;
    };

    if (!meshlets_.empty())
    {
        meshlet_buffer_ = make_device_buffer("assets.meshlets", meshlets_.data(),
                                             sizeof(GpuMeshlet) * meshlets_.size(), 0);
        meshlet_vertices_buffer_ =
            make_device_buffer("assets.meshlet_vertices", meshlet_vertices_.data(),
                               sizeof(uint32_t) * meshlet_vertices_.size(), 0);
        meshlet_triangles_buffer_ =
            make_device_buffer("assets.meshlet_triangles", meshlet_triangles_.data(),
                               sizeof(uint32_t) * meshlet_triangles_.size(), 0);
        view_.meshlets = fg.use_persistent(
            persistent_buffer_info{ .name = "assets.meshlets", .physical = { meshlet_buffer_ } });
        view_.meshlet_vertices = fg.use_persistent(persistent_buffer_info{
            .name = "assets.meshlet_vertices", .physical = { meshlet_vertices_buffer_ } });
        view_.meshlet_triangles = fg.use_persistent(persistent_buffer_info{
            .name = "assets.meshlet_triangles", .physical = { meshlet_triangles_buffer_ } });
    }

    // Brief 23: the compact skin heap, whole-uploaded like the meshlet heaps (never suballocated —
    // DrawInfo.skin_offset rebases ORIGINAL global indices, so geometry streaming never touches it).
    if (!skin_vertices_.empty())
    {
        skin_buffer_ = make_device_buffer(
            "assets.skin_stream", skin_vertices_.data(),
            sizeof(::string::asset::SkinVertex) * skin_vertices_.size(), 0);
        view_.skin_stream = fg.use_persistent(
            persistent_buffer_info{ .name = "assets.skin_stream", .physical = { skin_buffer_ } });
    }

    // The vertex heap: allocated whole up front but NOT uploaded here — the mesh heap uploads each
    // part's window on demand (or all up front at 100% residency). Sized to the SUM of per-part
    // windows, not the deduplicated vertex count: the heap suballocates a SEPARATE range per part,
    // so shared instanced vertices are duplicated.
    if (!mesh_parts_.empty())
    {
        uint64_t vertex_units = 0;
        for (const mesh_part& p : mesh_parts_) vertex_units += p.vertex_count;
        const uint64_t vertex_capacity =
            std::max<uint64_t>(1, vertex_units * config_.geometry_resident_percent / 100);
        vertex_buffer_ = make_device_buffer(
            "assets.vertices", nullptr, VkDeviceSize(sizeof(Vertex)) * vertex_capacity, 0);
        view_.vertices = fg.use_persistent(
            persistent_buffer_info{ .name = "assets.vertices", .physical = { vertex_buffer_ } });

        heap_ = std::make_unique<mesh_heap>(ctx_->allocator, ctx_->transfer, vertex_buffer_,
                                            std::span<const Vertex>(vertices_), vertex_capacity,
                                            ctx_->frames_in_flight);
        std::vector<mesh_heap::Window> windows;
        windows.reserve(mesh_parts_.size());
        for (const mesh_part& p : mesh_parts_) windows.push_back({ p.vertex_offset, p.vertex_count });
        heap_->set_windows(windows);

        geometry_streaming_ = config_.geometry_resident_percent < 100;
        if (geometry_streaming_)
        {
            const VkDeviceSize geometry_budget = VkDeviceSize(vertex_capacity) * sizeof(Vertex);
            geometry_residency_ = std::make_unique<::string::gpu::residency_manager>(
                geometry_budget, config_.geometry_stream_bytes_per_frame);
        }
    }
}

void registry::install_residency(std::function<void(uint32_t, uint32_t, uint32_t)> on_change)
{
    if (heap_ == nullptr) return;
    heap_->set_residency_callback(std::move(on_change));

    const uint32_t parts = static_cast<uint32_t>(mesh_parts_.size());
    if (geometry_streaming_)
    {
        // Doesn't fit: register with the manager and stream per-frame by visibility (tick()).
        for (uint32_t d = 0; d < parts; ++d)
        {
            geometry_residency_->register_resource(d, *heap_, /*min=*/0, /*max=*/1, /*initial=*/0);
        }
    }
    else
    {
        // Fits: upload every part's geometry now (drained once by the renderer's wait_idle before
        // frame 0) and reveal it. No per-frame streaming/eviction.
        for (uint32_t d = 0; d < parts; ++d)
        {
            heap_->stream(d, 0, 1);
            heap_->on_resident(d, 1);
        }
    }
}

void registry::begin_frame()
{
    // Textures: pinned to the coarse-tail floor; visibility raises them (finest request wins).
    for (std::size_t i = 0; i < texture_runtime_.size(); ++i)
    {
        frame_desired_detail_[i] = texture_runtime_[i].coarse_detail;  // 0 for stb textures
    }
    sampled_.assign(mesh_parts_.size(), 0);
    if (geometry_streaming_ && heap_ != nullptr)
    {
        heap_->begin_frame(stream_frame_);
    }
}

void registry::observe(std::span<const visibility_sample> samples)
{
    for (const visibility_sample& s : samples)
    {
        if (s.mesh.index >= mesh_parts_.size()) continue;
        sampled_[s.mesh.index] = 1;

        const mesh_part& p = mesh_parts_[s.mesh.index];
        if (!p.material.valid()) continue;
        const texture_id tex = materials_[p.material.index].base_color;
        if (!tex.valid()) continue;
        const texture_runtime& rt = texture_runtime_[tex.index];
        if (rt.levels == 0) continue;

        // Screen-coverage LOD: pick the mip whose texel count matches the projected span (texel:
        // pixel ~1). coverage_px == 0 means "behind the near plane / sub-pixel" -> the coarse floor.
        uint32_t want = rt.coarse_detail;
        if (s.coverage_px > 1.0f)
        {
            const float ratio = static_cast<float>(rt.base_extent) / s.coverage_px;
            uint32_t base_mip =
                ratio <= 1.0f ? 0u : static_cast<uint32_t>(std::floor(std::log2(ratio)));
            base_mip = std::min(base_mip, rt.levels - 1);
            want = std::max(rt.levels - base_mip, rt.coarse_detail);
        }
        frame_desired_detail_[tex.index] = std::max(frame_desired_detail_[tex.index], want);
    }
}

void registry::tick()
{
    // Geometry: want the sampled (visible) parts, release the rest so their heap space is reused.
    if (geometry_streaming_ && geometry_residency_ != nullptr)
    {
        for (uint32_t d = 0; d < mesh_parts_.size(); ++d)
        {
            if (sampled_[d])
                geometry_residency_->want(d, 1, ::string::gpu::resource_priority::LAZY,
                                          stream_frame_);
            else
                geometry_residency_->release(d);
        }
    }
    // Issue one want() per streamed texture with its aggregated desired detail. A texture wanted
    // above its coarse floor this frame is on screen and needs sharpening now → IMMEDIATE; one
    // still at the floor is background → LAZY (fills in without stalling the visible ones).
    for (std::size_t i = 0; i < texture_runtime_.size(); ++i)
    {
        if (texture_runtime_[i].levels == 0)
        {
            continue;  // non-streamed (stb) texture
        }
        const bool on_screen = frame_desired_detail_[i] > texture_runtime_[i].coarse_detail;
        const auto priority = on_screen ? ::string::gpu::resource_priority::IMMEDIATE
                                        : ::string::gpu::resource_priority::LAZY;
        texture_residency_->want(texture_runtime_[i].image, frame_desired_detail_[i], priority,
                                stream_frame_);
    }
    texture_residency_->tick(stream_frame_);
    if (geometry_streaming_ && geometry_residency_ != nullptr)
    {
        geometry_residency_->tick(stream_frame_);
        if (stream_frame_ == 1 || stream_frame_ == 5 || stream_frame_ == 60 || stream_frame_ == 300)
        {
            STRING_LOG_INFO("[geo] frame {}: {} of {} draws resident, {} MB streamed, {} evictions",
                            stream_frame_, heap_->resident_count(), mesh_parts_.size(),
                            heap_->streamed_bytes() / (1024 * 1024), heap_->evicted_count());
        }
    }

    // One-shot: report the frame at which every streamed texture reached full residency (all CACHED
    // at their desired detail), so streaming progress is observable against the load logs.
    if (!logged_full_resident_ && !streamed_.empty())
    {
        bool all_full = true;
        for (const ::string::gpu::resource_id id : streamed_)
        {
            if (texture_residency_->status_of(id) != ::string::gpu::stream_status::CACHED)
            {
                all_full = false;
                break;
            }
        }
        if (all_full)
        {
            STRING_LOG_INFO("[stream] all {} textures fully resident at frame {}",
                            streamed_.size(), stream_frame_);
            logged_full_resident_ = true;
        }
    }

    ++stream_frame_;
}

}  // namespace string::assets
