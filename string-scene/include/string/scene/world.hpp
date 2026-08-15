#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/core/gpu_types.hpp>

#include <string/scene/asset_handles.hpp>
#include <string/scene/asset_registry.hpp>
#include <string/scene/animator.hpp>
#include <string/scene/camera.hpp>
#include <string/scene/frame_view.hpp>

namespace string::scene
{

// A stable, generational handle to a spawned instance. Never an index into a caller-visible
// vector: despawning invalidates the generation, so a stale handle is detectably dead.
struct entity
{
    uint32_t index = ~0u;
    uint32_t generation = 0;
    bool valid() const { return index != ~0u; }
    bool operator==(const entity&) const = default;
};

struct light_id
{
    uint32_t index = ~0u;
    bool valid() const { return index != ~0u; }
    bool operator==(const light_id&) const = default;
};

// What to spawn. The entity is the WHOLE asset (spec: "Instance = mesh-part set + material refs +
// transform"); its mesh parts are internal rows, expanded renderer-side through the registry.
// Child entities exist for explicit attachments (mounts, props), not per part.
struct instance_desc
{
    assets::asset_id asset;
    glm::mat4 transform{ 1.0f };   // world (or local, when parented)
    entity parent{};               // transform hierarchy: this entity follows its parent
    std::string_view debug_name;
    uint64_t server_id = 0;        // the game's authoritative entity key (snapshot application)
};

struct light_desc
{
    LightType type = LightType::POINT;
    glm::vec3 position{ 0.0f };
    float range = 10.0f;
    glm::vec3 color{ 1.0f };
    float intensity = 100.0f;
    glm::vec3 direction{ 0.0f, -1.0f, 0.0f };   // spot only
    float inner_cos = 0.9f;                     // spot only
    float outer_cos = 0.8f;                     // spot only
};

// The WORLD: what exists, where it is, in flat handle-based SoA tables (spec: NOT ECS). Owns ZERO
// GPU objects and never sees an engine_context — a headless server can tick one. It retains every
// asset it spawns from (the registry outliving it) and publishes a value snapshot (frame_view)
// that the render bridge consumes; render passes never see this type.
class world
{
public:
    explicit world(assets::registry& assets) : assets_(assets) {}

    // --- content ---------------------------------------------------------------------------------
    entity spawn(const instance_desc& desc);
    void despawn(entity e);
    bool valid(entity e) const;

    // World transform for roots, local when parented (the hierarchy flushes in tick()).
    void set_transform(entity e, const glm::mat4& transform);
    glm::mat4 world_transform(entity e) const;
    void set_visible(entity e, bool visible);

    light_id add_light(const light_desc& desc);
    void set_light(light_id id, const light_desc& desc);
    void remove_light(light_id id);

    // --- animation (playback state is SCENE state; brief 23: state machines stay game code) -----
    // Cross-fade every animator of the entity to `clip`. Returns true if ANY animator had it.
    bool play(entity e, std::string_view clip, float fade_seconds = 0.15f);
    std::span<const std::unique_ptr<animator>> animators_of(entity e) const;
    // Renderer-bridge access by ROW index (frame_view::instance_row::entity_index).
    std::span<const std::unique_ptr<animator>> animators_at(uint32_t row_index) const;
    // Global playback-rate scale (a tuning lever the app mirrors from its cvars).
    void set_animation_rate(float rate) { animation_rate_ = rate; }

    // --- view + environment ----------------------------------------------------------------------
    // THE camera (moved out of the renderer's GeometryScene). The client drives it: bind controls,
    // call camera().update(input, dt, aspect) per frame, or set poses directly.
    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    environment& env() { return env_; }
    const environment& env() const { return env_; }
    // The presentation extent, pushed by the app each frame (the world cannot know the window).
    void set_viewport(uint32_t width, uint32_t height)
    {
        viewport_width_ = width;
        viewport_height_ = height;
    }

    // --- per-frame -------------------------------------------------------------------------------
    // Flush the transform hierarchy + refresh the published rows. (Animation sampling joins here
    // when playback state moves onto entities; sun/TOD when the environment moves out of the
    // renderer.)
    void tick(float dt);

    frame_view view() const;

    // --- enumeration (the inspector reads THESE — no parallel data model) ------------------------
    std::span<const entity> entities() const { return live_; }
    std::string_view name_of(entity e) const;
    uint64_t server_id_of(entity e) const;
    assets::asset_id asset_of(entity e) const;
    const assets::registry& assets() const { return assets_; }

private:
    struct row
    {
        assets::asset_id asset;
        glm::mat4 local{ 1.0f };
        glm::mat4 world{ 1.0f };
        entity parent{};
        std::string name;
        uint64_t server_id = 0;
        std::vector<std::unique_ptr<animator>> animators;   // one per skin of the asset
        uint32_t generation = 0;
        bool visible = true;
        bool alive = false;
    };
    const row* get(entity e) const;
    row* get(entity e);

    assets::registry& assets_;
    Camera camera_;
    environment env_;
    uint32_t viewport_width_ = 0;
    uint32_t viewport_height_ = 0;
    float animation_rate_ = 1.0f;
    std::vector<row> rows_;
    std::vector<uint32_t> free_;
    std::vector<entity> live_;                 // dense, spawn order (rebuilt on spawn/despawn)

    std::vector<light_desc> light_descs_;
    std::vector<GpuLight> lights_;             // GPU mirror, rebuilt when lights change
    std::vector<uint8_t> light_alive_;

    // Published snapshot storage (spans in frame_view point here).
    std::vector<instance_row> published_;
    uint64_t revision_ = 1;
    bool rows_dirty_ = true;
};

}  // namespace string::scene
