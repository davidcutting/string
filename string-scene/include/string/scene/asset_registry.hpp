#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <span>
#include <string_view>
#include <vector>

#include <string/anim/anim.hpp>
#include <string/core/vertex.hpp>
#include <string/gpu/residency_manager.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/asset/cooked_format.hpp>
#include <string/asset/cooked_scene.hpp>
#include <string/asset/geometry.hpp>

#include <string/scene/asset.hpp>
#include <string/scene/asset_handles.hpp>
#include <string/scene/mesh_heap.hpp>
#include <string/scene/texture_pool.hpp>

namespace string::assets
{

// Injected geometry cooker. The registry itself never links an importer (string-asset-tools is
// tools-only and does not ship); a dev/editor build installs a provider that cooks in-process, a
// shipped client passes nullptr and a missing/stale cook is a clean load_error instead.
// Precedent: SceneRegistry::set_rescan/set_cook already inject tooling from the app side.
class cook_provider
{
public:
    virtual ~cook_provider() = default;
    // Cook `source` into `out` (and best-effort write the cooked sidecar + manifest beside the
    // source — a failed write is non-fatal so read-only content folders still load). Returns false
    // only when the cook itself failed.
    virtual bool cook_geometry(const std::filesystem::path& source, uint32_t chunk_budget,
                               ::string::asset::CookedScene& out) = 0;
};

enum class load_error : uint8_t
{
    none,
    not_found,    // source file missing
    not_cooked,   // no fresh cooked sidecar and no cook_provider to make one
    malformed,    // cooked file exists but does not parse / cook failed
};

struct registry_config
{
    // Selects the cooked variant (`foo.c<budget>.cooked`); 0 = chunking off. Mirrors r.chunk.budget.
    uint32_t chunk_budget = 0;
    // Streaming budgets + policy (were geometry_pass constants; one owner, declared).
    VkDeviceSize texture_budget_bytes = 6ull * 1024 * 1024 * 1024;
    VkDeviceSize texture_stream_bytes_per_frame = 64ull * 1024 * 1024;
    // Vertex-heap capacity as a percentage of the loaded content's window sum; < 100 exercises
    // real geometry streaming/eviction, 100 keeps everything resident.
    uint64_t geometry_resident_percent = 100;
    VkDeviceSize geometry_stream_bytes_per_frame = 32ull * 1024 * 1024;
    // Streamed-texture LOD floor: the coarse tail whose finest level is ~this many pixels wide.
    uint32_t coarse_floor_pixels = 64;
};

struct load_stats
{
    uint32_t files = 0;
    uint32_t cooked_hits = 0;    // served from a fresh cooked file
    uint32_t cooked_misses = 0;  // cooked in-process via the provider
    double load_ms = 0.0;
};

// The graph-facing view of the registry's content heaps: the logical handles a render pass
// DECLARES reads of and resolves through its own pass_context (ctx.address). Minted once per
// graph authoring by declare(); invalid handles mean the content set has no such heap (no
// geometry / no skins).
struct gpu_view
{
    gpu::buffer vertices{};
    gpu::buffer meshlets{};
    gpu::buffer meshlet_vertices{};
    gpu::buffer meshlet_triangles{};
    gpu::buffer skin_stream{};
    uint32_t meshlet_capacity = 0;   // visbits sizing (1 bit per global meshlet id)
    uint64_t revision = 0;           // bumped on any load; consumers re-derive on change
};

// One frame's visibility feedback for a mesh part: it was inside the frustum, covering roughly
// `coverage_px` pixels (the projected-AABB span). The registry walks part -> material -> textures
// internally and drives both residency managers from this — the renderer never names a texture.
struct visibility_sample
{
    mesh_id mesh;
    float coverage_px = 0.0f;
};

// The runtime asset layer: stable ids -> loaded assets -> registry-GLOBAL content tables + their
// GPU backing. The cooked format's per-file index spaces are rebased exactly once, at insert;
// every table below is global and append-only for the life of the registry. The registry owns the
// content heaps (vertex/meshlet/skin), the texture pool (images + bindless slots), and BOTH
// residency managers; render passes consume the heaps through gpu_view handles and materials
// through resolved slots. Must outlive every consumer (streamers hand out views into its tables).
class registry
{
public:
    // The normal, GPU-backed shape.
    registry(engine_context& ctx, const registry_config& config, cook_provider* cook = nullptr);
    // CPU-only: parses + merges content tables but creates no GPU objects — unit tests, and a
    // headless server that ticks worlds against content tables with no device. declare()/
    // streaming are no-ops; texture slots resolve to 0.
    explicit registry(const registry_config& config, cook_provider* cook = nullptr);
    ~registry();

    registry(const registry&) = delete;
    registry& operator=(const registry&) = delete;

    // Load a cooked source (absolute path, or relative to cwd). Loading an already-loaded path
    // returns the existing id. On failure returns an invalid id and, if `err` is non-null, why.
    asset_id load(const std::filesystem::path& source, load_error* err = nullptr);

    // Insert an in-memory cooked scene (procedural content: lookdev, synthetic test sets, tests).
    // `name` is the registry key; `base_dir` resolves the scene's relative texture paths.
    asset_id load_baked(::string::asset::CookedScene&& scene, std::string_view name,
                        const std::filesystem::path& base_dir = {});

    const asset* get(asset_id) const;
    asset_id find(std::string_view name) const;
    std::span<const asset> loaded() const { return assets_; }

    // --- global content tables (rebased, append-only) -------------------------------------------
    std::span<const Vertex> vertices() const { return vertices_; }
    std::span<const ::string::asset::GpuMeshlet> meshlets() const { return meshlets_; }
    std::span<const uint32_t> meshlet_vertices() const { return meshlet_vertices_; }
    std::span<const uint32_t> meshlet_triangles() const { return meshlet_triangles_; }
    std::span<const ::string::asset::SkinVertex> skin_vertices() const { return skin_vertices_; }
    std::span<const glm::mat4> inverse_bind() const { return inverse_bind_; }
    std::span<const uint32_t> joint_remap() const { return joint_remap_; }
    std::span<const mesh_part> mesh_parts() const { return mesh_parts_; }
    std::span<const material> materials() const { return materials_; }
    std::span<const texture_desc> textures() const { return textures_; }
    std::span<const skin_binding> skins() const { return skins_; }

    const material& material_of(material_id id) const { return materials_[id.index]; }
    const skin_binding& skin_of(skin_id id) const { return skins_[id.index]; }

    // The deduped runtime animation set for a skin: one load per .anim pack, shared by every
    // skin and every spawned instance (the old app driver deserialized the pack once PER SKIN).
    // nullptr = no pack / failed load (callers degrade to bind pose).
    std::shared_ptr<::string::anim::AnimSet> anim_set(skin_id id);

    // Resolved bindless slots (stable under streaming: only the sampler's minLod moves).
    uint32_t texture_slot(texture_id id) const { return texture_runtime_[id.index].slot; }
    uint32_t white_slot() const { return pool_ ? pool_->white_slot() : 0; }
    uint32_t flat_normal_slot() const { return pool_ ? pool_->flat_normal_slot() : 0; }

    uint32_t total_meshlets() const { return static_cast<uint32_t>(meshlets_.size()); }
    const load_stats& stats() const { return stats_; }

    // --- GPU side --------------------------------------------------------------------------------
    // Mint the heap handles + create/upload the heap backing for everything loaded so far. Called
    // once per graph authoring, after this scene's loads and before any pass declares (the heaps'
    // capacity is FIXED here; later loads must fit inside it or fail clean). Uploads ride the
    // construction-upload flush.
    void declare(frame_graph& fg);
    const gpu_view& view() const { return view_; }

    // The renderer's DrawInfo residency gate: (part index, vertex_offset, resident). Installed
    // ONCE by the consumer that owns the draw table (after that table exists), because the up-front
    // residency loop / streaming registration runs here and its callbacks must land somewhere.
    void install_residency(std::function<void(uint32_t, uint32_t, uint32_t)> on_change);

    // --- per-frame streaming feedback ------------------------------------------------------------
    // Call in this order once per frame: begin_frame (reclaims elapsed deferred frees, resets the
    // per-frame desire), observe (any number of times), tick (issues wants + drives both managers,
    // then advances the internal frame counter).
    void begin_frame();
    void observe(std::span<const visibility_sample> samples);
    void tick();

private:
    asset_id insert(::string::asset::CookedScene&& scene, std::string_view name,
                    const std::filesystem::path& base_dir,
                    const std::filesystem::path& anim_pack);
    void register_textures(uint32_t first_texture, uint32_t count,
                           std::span<const ::string::asset::CookedTexture> cooked);

    engine_context* ctx_ = nullptr;   // null = CPU-only (tests / headless)
    registry_config config_;
    cook_provider* cook_ = nullptr;

    std::vector<asset> assets_;

    std::vector<Vertex> vertices_;
    std::vector<::string::asset::GpuMeshlet> meshlets_;
    std::vector<uint32_t> meshlet_vertices_;
    std::vector<uint32_t> meshlet_triangles_;
    std::vector<::string::asset::SkinVertex> skin_vertices_;
    std::vector<glm::mat4> inverse_bind_;
    std::vector<uint32_t> joint_remap_;
    std::vector<mesh_part> mesh_parts_;
    std::vector<material> materials_;
    std::vector<texture_desc> textures_;
    std::vector<skin_binding> skins_;

    load_stats stats_;
    std::unordered_map<std::string, std::shared_ptr<::string::anim::AnimSet>> anim_cache_;

    // --- texture runtime (index-aligned with textures_) -----------------------------------------
    struct texture_runtime
    {
        ::string::gpu::resource_id image = 0;
        uint32_t slot = 0;
        uint32_t levels = 0;        // 0 = whole-uploaded (stb), not streamed
        uint32_t base_extent = 0;
        uint32_t coarse_detail = 0;
    };
    std::unique_ptr<texture_pool> pool_;
    std::unique_ptr<::string::gpu::residency_manager> texture_residency_;
    std::vector<texture_runtime> texture_runtime_;
    std::vector<::string::gpu::resource_id> streamed_;
    std::vector<uint32_t> frame_desired_detail_;
    std::vector<uint8_t> sampled_;
    bool logged_full_resident_ = false;

    // --- geometry runtime (created at declare) ---------------------------------------------------
    ::string::gpu::resource_id vertex_buffer_ = 0;
    ::string::gpu::resource_id meshlet_buffer_ = 0;
    ::string::gpu::resource_id meshlet_vertices_buffer_ = 0;
    ::string::gpu::resource_id meshlet_triangles_buffer_ = 0;
    ::string::gpu::resource_id skin_buffer_ = 0;
    std::unique_ptr<mesh_heap> heap_;
    std::unique_ptr<::string::gpu::residency_manager> geometry_residency_;
    bool geometry_streaming_ = false;
    gpu_view view_;

    uint64_t stream_frame_ = 0;
};

}  // namespace string::assets
