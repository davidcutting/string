#include <algorithm>
#include <vector>

#include <string/gpu/residency_manager.hpp>

namespace string::gpu
{

namespace
{
std::uint32_t clamp_detail(std::uint32_t detail, std::uint32_t lo, std::uint32_t hi)
{
    return std::max(lo, std::min(detail, hi));
}
}  // namespace

residency_manager::residency_manager(VkDeviceSize budget_bytes, VkDeviceSize max_stream_bytes_per_tick)
: budget_(budget_bytes)
, max_stream_bytes_per_tick_(max_stream_bytes_per_tick)
{
}

void residency_manager::register_resource(resource_id id, residency_provider& provider,
    std::uint32_t min_detail, std::uint32_t max_detail, std::uint32_t initial_detail)
{
    const std::uint32_t resident = clamp_detail(initial_detail, min_detail, max_detail);
    entry e;
    e.id = id;
    e.provider = &provider;
    e.min_detail = min_detail;
    e.max_detail = max_detail;
    e.resident = resident;
    e.desired = resident;
    e.streaming_to = resident;
    e.status = stream_status::CACHED;
    entries_[id] = e;
}

void residency_manager::unregister_resource(resource_id id)
{
    entries_.erase(id);
}

void residency_manager::want(resource_id id, std::uint32_t detail, resource_priority priority,
    std::uint64_t frame)
{
    auto it = entries_.find(id);
    if (it == entries_.end())
    {
        return;
    }
    entry& e = it->second;
    e.desired = clamp_detail(detail, e.min_detail, e.max_detail);
    e.priority = priority;
    e.last_visible_frame = frame;
}

void residency_manager::release(resource_id id)
{
    auto it = entries_.find(id);
    if (it == entries_.end())
    {
        return;
    }
    it->second.desired = it->second.min_detail;
}

VkDeviceSize residency_manager::committed_cost(entry& e) const
{
    // While streaming, the target detail is already reserved so the budget accounts for in-flight
    // uploads before they land; otherwise the resident detail is what occupies VRAM.
    const std::uint32_t detail = (e.status == stream_status::STREAMING) ? e.streaming_to : e.resident;
    return e.provider->cost(e.id, detail);
}

VkDeviceSize residency_manager::resident_bytes() const
{
    VkDeviceSize total = 0;
    for (const auto& [id, e] : entries_)
    {
        const std::uint32_t detail = (e.status == stream_status::STREAMING) ? e.streaming_to : e.resident;
        total += e.provider->cost(e.id, detail);
    }
    return total;
}

bool residency_manager::evict_one(std::uint64_t frame)
{
    // Least-recently-visible evictable entry: CACHED, above its pinned floor, and not requested at
    // its current detail this frame (evicting a resource still visible this frame would just thrash
    // — it would re-want immediately). STREAMING entries are skipped (their upload is in flight).
    entry* victim = nullptr;
    for (auto& [id, e] : entries_)
    {
        if (e.status != stream_status::CACHED) continue;
        if (e.resident <= e.min_detail) continue;
        // IMMEDIATE resources must stay resident (correctness over the cap); only LAZY detail is
        // reclaimable. And don't evict something requested at its current detail this very frame —
        // it would just re-want next tick and thrash.
        if (e.priority == resource_priority::IMMEDIATE) continue;
        if (e.last_visible_frame == frame && e.desired >= e.resident) continue;
        if (victim == nullptr || e.last_visible_frame < victim->last_visible_frame)
        {
            victim = &e;
        }
    }
    if (victim == nullptr)
    {
        return false;
    }
    victim->provider->evict(victim->id, victim->resident, victim->min_detail);
    victim->resident = victim->min_detail;
    victim->streaming_to = victim->min_detail;
    victim->status = (victim->desired > victim->resident) ? stream_status::WANTED : stream_status::CACHED;
    return true;
}

void residency_manager::tick(std::uint64_t frame)
{
    // 1. Complete in-flight streams whose GPU upload has landed.
    for (auto& [id, e] : entries_)
    {
        if (e.status == stream_status::STREAMING && e.provider->is_complete(e.ticket))
        {
            e.resident = e.streaming_to;
            e.ticket = 0;
            e.provider->on_resident(e.id, e.resident);
            e.status = (e.desired > e.resident) ? stream_status::WANTED : stream_status::CACHED;
        }
    }

    // 2. Honour demotions immediately (desired dropped below resident, e.g. release()) — this frees
    //    budget before we consider evictions/promotions. Never below the pinned floor.
    for (auto& [id, e] : entries_)
    {
        if (e.status != stream_status::CACHED) continue;
        const std::uint32_t target = std::max(e.desired, e.min_detail);
        if (target < e.resident)
        {
            e.provider->evict(e.id, e.resident, target);
            e.resident = target;
            e.streaming_to = target;
        }
    }

    // 3. Enforce the budget: evict least-recently-visible finer detail until under budget or nothing
    //    more can be freed (all remaining are pinned/streaming/visible — accept the overage rather
    //    than thrash).
    while (resident_bytes() > budget_)
    {
        if (!evict_one(frame))
        {
            break;
        }
    }

    // 4. Mark resources that want finer detail than they have as WANTED (unless already streaming).
    for (auto& [id, e] : entries_)
    {
        if (e.status == stream_status::CACHED && e.desired > e.resident)
        {
            e.status = stream_status::WANTED;
        }
    }

    // 5. Promote WANTED -> STREAMING, most-recently-visible (and IMMEDIATE) first, but only while the
    //    stream fits the budget. IMMEDIATE resources stream regardless (they must be available).
    std::vector<entry*> wanted;
    for (auto& [id, e] : entries_)
    {
        if (e.status == stream_status::WANTED && e.desired > e.resident)
        {
            wanted.push_back(&e);
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const entry* a, const entry* b) {
        if (a->priority != b->priority) return a->priority == resource_priority::IMMEDIATE;
        return a->last_visible_frame > b->last_visible_frame;
    });
    VkDeviceSize started_this_tick = 0;
    for (entry* e : wanted)
    {
        const VkDeviceSize delta =
            e->provider->cost(e->id, e->desired) - e->provider->cost(e->id, e->resident);
        const bool immediate = e->priority == resource_priority::IMMEDIATE;
        const bool fits = resident_bytes() + delta <= budget_;
        if (!fits && !immediate)
        {
            continue;  // stays WANTED; a later frame (after eviction / release) may make room
        }
        // The provider may lack backing space (suballocator full/fragmented) even when the byte
        // budget says yes; leave it WANTED and retry once eviction frees a usable range.
        if (!e->provider->can_stream(e->id, e->desired))
        {
            continue;
        }
        // Spread new streaming across frames: don't exceed this tick's byte budget, but always allow
        // at least one stream so progress is guaranteed even for a resource larger than the cap.
        // IMMEDIATE bypasses the per-tick cap.
        if (!immediate && max_stream_bytes_per_tick_ != 0 && started_this_tick != 0 &&
            started_this_tick + delta > max_stream_bytes_per_tick_)
        {
            continue;
        }
        e->ticket = e->provider->stream(e->id, e->resident, e->desired);
        e->streaming_to = e->desired;
        e->status = stream_status::STREAMING;
        started_this_tick += delta;
    }
}

stream_status residency_manager::status_of(resource_id id) const
{
    auto it = entries_.find(id);
    return it == entries_.end() ? stream_status::UNWANTED : it->second.status;
}

std::uint32_t residency_manager::resident_detail(resource_id id) const
{
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.resident;
}

}  // namespace string::gpu
