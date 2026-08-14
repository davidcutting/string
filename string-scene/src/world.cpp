#include <string/scene/world.hpp>

#include <string/core/logger.hpp>

namespace string::scene
{

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

void world::tick(float /*dt*/)
{
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
    return frame_view{ .instances = published_, .lights = lights_, .revision = revision_ };
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

}  // namespace string::scene
