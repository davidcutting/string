#pragma once

#include <cstdint>
#include <unordered_map>

#include <string/gpu/resource.hpp>

#include <volk.h>

namespace string::gpu
{

// Generic asset-residency policy engine, keyed on resource_id, shared by textures and geometry.
//
// A resource has a "detail" level: a monotonic integer where HIGHER = finer / more resident data
// (for a texture it's a mip count, for a mesh it's simply 0 = absent, 1 = resident). The manager
// tracks, per resource, what detail the app wants (want()) versus what's resident, and drives it
// toward the target within a fixed VRAM budget — streaming finer detail in on demand and evicting
// the least-recently-visible resources' finer detail when over budget. A pinned coarse floor
// (min_detail) is never evicted, so every resource always renders (blurry / low-LOD) rather than
// popping to nothing.
//
// The manager is pure policy: it knows nothing about Vulkan uploads or KTX or vertex buffers. All
// GPU work is delegated to a residency_provider the caller implements (in the sandbox), which the
// manager calls to stream detail in, ask whether an upload finished, publish it, evict, and price
// it. This keeps the engine layer free of format/streaming specifics and makes the policy
// unit-testable with a fake provider.
class residency_provider
{
public:
    virtual ~residency_provider() = default;

    // Record the GPU work to raise `id` from `from_detail` to the finer `to_detail` (e.g. upload
    // the newly-needed mips / the mesh's vertices). Returns an opaque ticket the manager later
    // polls via is_complete(); return 0 if the data is already resident (nothing to wait on).
    virtual std::uint64_t stream(resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) = 0;
    // Whether a ticket returned by stream() has completed on the GPU. 0 is always complete.
    virtual bool is_complete(std::uint64_t ticket) = 0;
    // The streamed detail is now resident on the GPU: make it visible (e.g. lower sampler minLod).
    virtual void on_resident(resource_id id, std::uint32_t detail) = 0;
    // Coarsen `id` from `from_detail` down to `to_detail` immediately (drop/mask finer detail so it
    // is no longer sampled; the provider may free the backing memory). No GPU wait.
    virtual void evict(resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) = 0;
    // VRAM bytes `id` occupies when resident at `detail`. Must be monotonic in detail.
    virtual VkDeviceSize cost(resource_id id, std::uint32_t detail) = 0;
    // Whether the provider can actually back `to_detail` right now (e.g. its suballocator has room).
    // The manager will not promote a WANTED resource to STREAMING while this is false — it stays
    // WANTED and retries after eviction frees space. Defaults to true (providers that always can).
    virtual bool can_stream(resource_id /*id*/, std::uint32_t /*to_detail*/) { return true; }
};

class residency_manager
{
public:
    // budget_bytes caps total resident VRAM (LRU eviction keeps under it). max_stream_bytes_per_tick
    // rate-limits how many *new* bytes may begin streaming per tick() so a burst of newly-visible
    // detail is spread across frames instead of stalling one; 0 means unlimited. IMMEDIATE streams
    // ignore the per-tick cap (they must land ASAP).
    explicit residency_manager(VkDeviceSize budget_bytes, VkDeviceSize max_stream_bytes_per_tick = 0);

    // Register a resource whose coarse floor (min_detail..initial_detail) is already resident.
    // max_detail is the finest available; min_detail is the pinned floor that is never evicted.
    void register_resource(resource_id id, residency_provider& provider, std::uint32_t min_detail,
                           std::uint32_t max_detail, std::uint32_t initial_detail);
    void unregister_resource(resource_id id);

    // Called each frame for every visible resource: request `detail` (clamped to [min,max]) and
    // stamp it visible at `frame` (drives LRU). Resources not want()'d this frame keep their prior
    // request but grow stale, so they are evicted first under budget pressure.
    void want(resource_id id, std::uint32_t detail, resource_priority priority, std::uint64_t frame);
    // Explicitly stop wanting finer detail (drops the request to the pinned floor).
    void release(resource_id id);

    // Advance the machine one step: complete finished streams, honour demotions, enforce the
    // budget by evicting the least-recently-visible finer detail, then start new streams that fit.
    void tick(std::uint64_t frame);

    // Introspection (for tests / logging / feedback).
    stream_status status_of(resource_id id) const;
    std::uint32_t resident_detail(resource_id id) const;
    VkDeviceSize resident_bytes() const;
    VkDeviceSize budget() const { return budget_; }

private:
    struct entry
    {
        resource_id id = 0;
        residency_provider* provider = nullptr;
        std::uint32_t min_detail = 0;
        std::uint32_t max_detail = 0;
        std::uint32_t resident = 0;      // detail currently resident on the GPU
        std::uint32_t desired = 0;       // detail the app last requested
        std::uint32_t streaming_to = 0;  // target of the in-flight stream (valid while STREAMING)
        stream_status status = stream_status::CACHED;
        resource_priority priority = resource_priority::LAZY;
        std::uint64_t ticket = 0;
        std::uint64_t last_visible_frame = 0;
    };

    // Bytes an entry currently commits to VRAM: while streaming it already reserves the target
    // detail's cost (so promotions don't over-commit the budget before completion).
    VkDeviceSize committed_cost(entry& e) const;
    // Evict the single least-recently-visible evictable entry's finer detail down to its floor.
    // Returns false when nothing more can be evicted (all pinned / streaming / visible this frame).
    bool evict_one(std::uint64_t frame);

    VkDeviceSize budget_;
    VkDeviceSize max_stream_bytes_per_tick_;
    std::unordered_map<resource_id, entry> entries_;
};

}  // namespace string::gpu
