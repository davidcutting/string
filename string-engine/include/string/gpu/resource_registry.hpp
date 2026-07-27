#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/resource.hpp>
#include <string/vulkan/frame_scratch.hpp>

#include <volk.h>

namespace string::gpu
{

class resource_allocator;
class descriptor_table;

// Brief 16 — Layer 1 (resource virtualization). The logical, author-facing resource vocabulary.
//
//   image / buffer        logical, TYPED handle — the only resource type pass code names
//        |  ResourceRegistry maps logical -> physical, by Lifetime + frame_slot
//   resource_id           physical slot handle — untyped (the allocator is kind-agnostic)
//        |  allocator maps slot -> concrete
//   allocated_image / allocated_buffer   VkImage/VkBuffer + view + VMA
//
// The clean names sit on the LOGICAL layer (a deliberate inversion of the usual convention) because
// with full virtualization the author only ever touches the logical layer; `allocated_*` already
// claims the concrete-backing qualifier. `image`/`buffer` are opaque indices into a ResourceRegistry
// — never a raw resource_id, never a VkImage. (Handle<*> is retired; its typing job lives here now.)

// How the registry backs a logical resource physically. M0 implements only `Imported`; the rest are
// declared now so the vocabulary is complete and later milestones (M1 PerFrame, M5 Transient, M6
// Streamed) slot in without churning the enum.
enum class Lifetime : std::uint8_t
{
    Persistent,  // one resource_id, lives across frames (visibility bitfield, meshlet heaps, shadow maps)
    PerFrame,    // resource_id[frame_slot] — a ring; resolution needs the current slot (SceneData, hiz[])
    Transient,   // a resource_id from an aliased pool; resolution needs the slot (worklists, draw_lod)
    Imported,    // an externally-owned resource_id, viewport-sized (swapchain, msaa color/depth, HDR target)
    Streamed,    // residency-managed resource_id(s) at the current LOD (textures, geometry heaps)
};

// Brief 16 M6 — `Streamed` is DELEGATE-FIRST (user-confirmed scoping): streamed resources remain owned
// + driven by the existing `residency_manager` (per-resource want/release/tick/status, keyed on
// resource_id) — the registry does NOT re-implement residency. The residency_manager IS the delegate.
// Folding it physically under the registry (a unified want/unwant front-door + Streamed handles that
// resolve to the current-LOD physical) is the LATER ORTHOGONAL step the brief names as out of scope for
// this pass; the vocabulary is registered here so the layering is complete and that step slots in
// without churning the enum. See residency_manager.hpp + GeometryPass's texture/geometry residency.

// Logical, typed handles. Distinct types so image != buffer at compile time (this is where the old
// Handle<Image>/Handle<Buffer> typing job relocated). The stored value is an index into the owning
// ResourceRegistry's table, NOT a resource_id — do not construct these by hand.
struct image
{
    static constexpr std::uint32_t invalid = ~std::uint32_t{ 0 };
    std::uint32_t index = invalid;
    bool valid() const { return index != invalid; }
};

struct buffer
{
    static constexpr std::uint32_t invalid = ~std::uint32_t{ 0 };
    std::uint32_t index = invalid;
    bool valid() const { return index != invalid; }
};

// Layer-1 hub: owns the logical -> physical mapping and is the single authority the execution layer
// resolves through. M0 is the skeleton: it wraps the allocator + descriptor_table and implements the
// `Imported` lifetime only (the renderer's stable render targets). Later milestones grow it with
// image()/buffer() declaration, materialize/plan_transients/recreate_viewport/collect_garbage, and
// the PerFrame/Transient/Streamed backings.
class ResourceRegistry
{
public:
    ResourceRegistry(resource_allocator& allocator, descriptor_table& descriptor_table);
    // Destroys the physical backing of resources the registry OWNS (PerFrame rings, and later
    // Persistent/Transient). Imported resources are externally owned and left untouched. Runs while
    // the allocator is still alive (the registry is declared after it, so destroyed before it).
    ~ResourceRegistry();

    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;

    // --- images ----------------------------------------------------------------------------------
    // Register an externally-owned physical target (Lifetime::Imported). The registry references the
    // physical id but does not own it — the caller keeps creating/destroying it (e.g. the renderer's
    // viewport-sized attachments, recreated on resize -> re-point via reimport()).
    image import_image(resource_id physical);

    // Re-point an imported handle at a freshly-created physical id. The `Imported` analogue of
    // recreate_viewport(): after a resize destroys+recreates a viewport-sized target, its resource_id
    // changes, so the handle must be updated to track the new backing.
    void reimport(image handle, resource_id physical);

    // M7 (#1): allocate + own a PerFrame IMAGE ring (one physical per frame-in-flight). The registry
    // is the allocation authority for the ring (create + free); the CALLER keeps its technique-specific
    // descriptor wiring (sampled/storage bindless slots, per-mip views) over the resolved physicals.
    // Viewport-sized rings that grow on resize re-point via recreate_per_frame_image (device-idle-safe).
    image create_per_frame_image(const image_info& info, std::uint32_t frames_in_flight);
    void  recreate_per_frame_image(image handle, const image_info& info);

    // Resolution. The no-slot forms are for single-physical (Imported/Persistent) images; the slot
    // forms resolve a PerFrame ring. (In M0, the renderer's image_of/view_of use the no-slot forms.)
    resource_id physical(image handle) const;
    VkImageView view(image handle) const;
    resource_id physical(image handle, std::uint32_t slot) const;
    VkImageView view(image handle, std::uint32_t slot) const;
    Lifetime    lifetime(image handle) const;

    // --- buffers ---------------------------------------------------------------------------------
    // M1: allocate + own a PerFrame buffer ring (one physical per frame-in-flight). The registry
    // creates them from `info` immediately (M1 does not yet defer to a materialize() pass) and owns
    // them for its lifetime. Resolution needs the current frame slot. Used for the SceneData ring;
    // later the light/stats rings (M3). For a host-visible mapped ring pass `info` with the mapped
    // allocation flags and read the per-slot pointer via mapped().
    buffer create_per_frame(const buffer_info& info, std::uint32_t frames_in_flight);

    // Reallocate an existing PerFrame ring in place (same handle, same frame count): destroys the
    // current physical ids and creates fresh ones from `info`. For a viewport-sized PerFrame buffer
    // that grows on resize (the froxel index ring). The caller device-waits idle before calling (the
    // renderer does on resize), so the old backing is safe to free.
    void recreate_per_frame(buffer handle, const buffer_info& info);

    resource_id     physical(buffer handle, std::uint32_t slot) const;
    VkDeviceAddress address (buffer handle, std::uint32_t slot) const;
    void*           mapped  (buffer handle, std::uint32_t slot) const;
    Lifetime        lifetime(buffer handle) const;

    // --- transients (Lifetime::Transient) --------------------------------------------------------
    // Brief 16 M5: the registry is the transient authority. It OWNS the per-frame-slot scratch arena
    // (FrameScratch — the transient-aliasing arena passes reserve worklists/draw_lod/histogram
    // regions into). Passes reserve at construction via transients().reserve(); the renderer calls
    // materialize_transients() after all passes are built; the registry frees it in its dtor.
    // (Genuine greedy interval-packing memory REUSE is a no-op today — all current transients overlap
    // the geometry pass, so the arena packs them disjoint, exactly as the brief specifies; per-resource
    // Transient HANDLES + cross-lifetime reuse are a deferred follow-up. String namespace stays for M7.)
    FrameScratch& transients() { return transients_; }
    void materialize_transients(std::uint32_t frame_slots) { transients_.materialize(allocator_, frame_slots); }

private:
    struct image_slot
    {
        Lifetime lifetime;
        std::vector<resource_id> physical;   // Imported/Persistent: size 1; PerFrame: one per frame slot
        bool owned = false;                  // registry allocated + destroys these (PerFrame) vs Imported
    };

    struct buffer_slot
    {
        Lifetime lifetime;
        std::vector<resource_id> physical;   // PerFrame: one id per frame slot (Persistent: size 1)
        bool owned = false;                  // registry destroys these in its dtor
    };

    resource_allocator& allocator_;
    descriptor_table& descriptor_table_;
    std::vector<image_slot> images_;
    std::vector<buffer_slot> buffers_;
    FrameScratch transients_;   // owned per-frame-slot transient arena (materialized by the renderer)
};

}  // namespace string::gpu
