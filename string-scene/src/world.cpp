#include <string/scene/world.hpp>

#include <cmath>

#include <glm/gtc/constants.hpp>

#include <string/core/logger.hpp>

namespace string::scene
{
namespace
{

// Time-of-day 0..1 -> a sun direction arc (east low -> zenith -> west low) and the sky/sun
// palette. Moved from the renderer with the environment: this is world policy, and every renderer
// consumes its OUTPUT (env.sun_dir + palette), never the time directly.
struct sun_state
{
    glm::vec3 dir;
    glm::vec3 sun_color;
    float sun_intensity;
    glm::vec3 sky_zenith;
    glm::vec3 sky_ground;
};
sun_state sun_for_time(float t, float lean)
{
    // Great-circle day path: the sun rides a single TILTED CIRCLE from the east horizon, arcing up
    // and leaning south, down to the west horizon. `lean` tilts the arc toward south: smaller =
    // higher noon sun (~0 = straight overhead), larger = a lower, flatter arc.
    const float p = t * glm::pi<float>();               // 0 (east horizon) .. pi (west horizon)
    glm::vec3 d(-std::cos(p),                            // east -> west
                std::sin(p) * std::cos(lean),            // up (arcs over the day)
                std::sin(p) * std::sin(lean));           // south lean
    d.y = d.y * 0.94f + 0.06f;                           // keep the "never fully below horizon" floor
    sun_state s;
    s.dir = glm::normalize(d);
    // Warm at the horizon (sunrise/sunset), neutral-bright at noon. Drive off the actual elevation.
    const float noon = glm::clamp(s.dir.y, 0.0f, 1.0f);
    s.sun_color = glm::mix(glm::vec3(1.0f, 0.55f, 0.28f), glm::vec3(1.0f, 0.96f, 0.9f), noon);
    // Brief 07 M4 units (self-consistent, "physical-ish"): illuminance in KILOLUX, luminance /
    // radiance in KILO-NITS. Sun: ~100 klx perpendicular at noon, ~7 klx at the horizon.
    s.sun_intensity = glm::mix(7.0f, 100.0f, noon);
    s.sky_zenith = glm::mix(glm::vec3(0.24f, 0.40f, 0.96f), glm::vec3(2.8f, 6.0f, 12.4f), noon);
    // Ground band is a constant ALBEDO; its radiance is derived per-direction in the shader from
    // the CURRENT sun + sky (sky_ground_radiance in sky.slang).
    s.sky_ground = glm::vec3(0.0824f, 0.0699f, 0.0503f);
    return s;
}

}  // namespace

entity world::spawn(const instance_desc& desc)
{
    if (assets_.get(desc.asset) == nullptr)
    {
        STRING_LOG_WARN("[world] spawn with an invalid asset id — ignored");
        return {};
    }
    uint32_t index;
    if (!free_.empty())
    {
        index = free_.back();
        free_.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(rows_.size());
        rows_.emplace_back();
    }
    row& r = rows_[index];
    r.asset = desc.asset;
    r.local = desc.transform;
    r.world = desc.transform;
    r.parent = desc.parent;
    r.name = std::string(desc.debug_name);
    r.server_id = desc.server_id;
    r.visible = true;
    r.alive = true;

    // One animator per skin of the asset (deduped AnimSet from the registry). An asset without
    // skins costs nothing; a broken pack leaves the animator invalid = bind pose.
    r.animators.clear();
    if (const assets::asset* a = assets_.get(desc.asset); a != nullptr && a->skin_count() > 0)
    {
        for (uint32_t s = 0; s < a->skin_count(); ++s)
        {
            const assets::skin_id sid{ a->first_skin().index + s };
            const assets::skin_binding& sb = assets_.skin_of(sid);
            r.animators.push_back(std::make_unique<animator>(assets_.anim_set(sid), sid,
                                                             sb.skeleton_hash));
        }
    }

    const entity e{ .index = index, .generation = r.generation };
    live_.push_back(e);
    rows_dirty_ = true;
    ++revision_;
    return e;
}

void world::despawn(entity e)
{
    row* r = get(e);
    if (r == nullptr) return;
    r->alive = false;
    r->animators.clear();
    ++r->generation;   // stale handles die here
    free_.push_back(e.index);
    std::erase(live_, e);
    rows_dirty_ = true;
    ++revision_;
}

bool world::valid(entity e) const
{
    return get(e) != nullptr;
}

void world::set_transform(entity e, const glm::mat4& transform)
{
    if (row* r = get(e))
    {
        r->local = transform;
    }
}

glm::mat4 world::world_transform(entity e) const
{
    const row* r = get(e);
    return r != nullptr ? r->world : glm::mat4(1.0f);
}

void world::set_visible(entity e, bool visible)
{
    if (row* r = get(e); r != nullptr && r->visible != visible)
    {
        r->visible = visible;
        rows_dirty_ = true;
        ++revision_;
    }
}

light_id world::add_light(const light_desc& desc)
{
    light_descs_.push_back(desc);
    light_alive_.push_back(1);
    rows_dirty_ = true;
    ++revision_;
    return light_id{ static_cast<uint32_t>(light_descs_.size()) - 1 };
}

void world::set_light(light_id id, const light_desc& desc)
{
    if (id.index < light_descs_.size() && light_alive_[id.index])
    {
        light_descs_[id.index] = desc;
        rows_dirty_ = true;   // lights_ mirrors rebuild in tick
    }
}

void world::remove_light(light_id id)
{
    if (id.index < light_descs_.size() && light_alive_[id.index])
    {
        light_alive_[id.index] = 0;
        rows_dirty_ = true;
        ++revision_;
    }
}

void world::tick(float dt)
{
    // Animation: advance every live animator (fade + sample). Palette WRITES stay renderer-side.
    for (const entity e : live_)
        for (const std::unique_ptr<animator>& a : rows_[e.index].animators)
            if (a) a->tick(dt, animation_rate_);

    // Environment: advance the day when animating, then derive the sun + palette from the time.
    if (env_.animate_sun) env_.time_of_day = std::fmod(env_.time_of_day + dt * 0.03f, 1.0f);
    const sun_state sun = sun_for_time(env_.time_of_day, env_.sun_lean);
    env_.sun_dir = sun.dir;
    env_.sun_color = sun.sun_color;
    env_.sun_intensity = sun.sun_intensity;
    env_.sky_zenith = sun.sky_zenith;
    env_.sky_ground = sun.sky_ground;

    // Hierarchy flush: parents are spawned before children in live_ order for the common case, but
    // a re-parent could break that, so resolve each chain explicitly (depth is tiny — mounts and
    // attachments, not skeletons; those live in the animation system).
    for (const entity e : live_)
    {
        row& r = rows_[e.index];
        if (!r.parent.valid())
        {
            r.world = r.local;
            continue;
        }
        const row* p = get(r.parent);
        r.world = p != nullptr ? p->world * r.local : r.local;
    }

    // Publish the snapshot rows. Rebuild the vector only when the SET changed; transforms are
    // refreshed in place every tick (they are the hot path — players moving).
    if (rows_dirty_)
    {
        published_.clear();
        published_.reserve(live_.size());
        for (const entity e : live_)
        {
            const row& r = rows_[e.index];
            published_.push_back(instance_row{ .asset = r.asset,
                                               .transform = r.world,
                                               .visible = r.visible ? 1u : 0u,
                                               .entity_index = e.index });
        }
        lights_.clear();
        for (std::size_t i = 0; i < light_descs_.size(); ++i)
        {
            if (!light_alive_[i]) continue;
            const light_desc& d = light_descs_[i];
            lights_.push_back(GpuLight{
                .position_radius = glm::vec4(d.position, d.range),
                .color_intensity = glm::vec4(d.color, d.intensity),
                .direction_type = glm::vec4(d.direction, static_cast<float>(d.type)),
                .cone = glm::vec4(d.inner_cos, d.outer_cos, 0.0f, 0.0f),
            });
        }
        rows_dirty_ = false;
    }
    else
    {
        for (std::size_t i = 0; i < published_.size(); ++i)
        {
            published_[i].transform = rows_[published_[i].entity_index].world;
        }
    }
}

frame_view world::view() const
{
    return frame_view{
        .view = camera_.view(),
        .view_proj = camera_.view_proj(),
        .camera_pos = camera_.position(),
        .near_plane = camera_.near_plane(),
        .far_plane = camera_.far_plane(),
        .fov_degrees = camera_.fov_degrees(),
        .aspect = camera_.aspect(),
        .viewport_width = viewport_width_,
        .viewport_height = viewport_height_,
        .env = env_,
        .instances = published_,
        .lights = lights_,
        .revision = revision_,
    };
}

std::string_view world::name_of(entity e) const
{
    const row* r = get(e);
    return r != nullptr ? std::string_view(r->name) : std::string_view{};
}

uint64_t world::server_id_of(entity e) const
{
    const row* r = get(e);
    return r != nullptr ? r->server_id : 0;
}

assets::asset_id world::asset_of(entity e) const
{
    const row* r = get(e);
    return r != nullptr ? r->asset : assets::asset_id{};
}

const world::row* world::get(entity e) const
{
    if (!e.valid() || e.index >= rows_.size()) return nullptr;
    const row& r = rows_[e.index];
    return (r.alive && r.generation == e.generation) ? &r : nullptr;
}

world::row* world::get(entity e)
{
    return const_cast<row*>(static_cast<const world*>(this)->get(e));
}

bool world::play(entity e, std::string_view clip, float fade_seconds)
{
    row* r = get(e);
    if (r == nullptr) return false;
    bool any = false;
    for (const std::unique_ptr<animator>& a : r->animators)
        if (a && a->play(clip, fade_seconds)) any = true;
    return any;
}

std::span<const std::unique_ptr<animator>> world::animators_of(entity e) const
{
    const row* r = get(e);
    return r != nullptr ? std::span<const std::unique_ptr<animator>>(r->animators)
                        : std::span<const std::unique_ptr<animator>>{};
}

std::span<const std::unique_ptr<animator>> world::animators_at(uint32_t row_index) const
{
    if (row_index >= rows_.size() || !rows_[row_index].alive)
        return {};
    return rows_[row_index].animators;
}

}  // namespace string::scene
