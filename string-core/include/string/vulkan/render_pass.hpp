#pragma once

#include <string/gpu/command_recorder.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <functional>
#include <string_view>
#include <vector>

#include <volk.h>

namespace String
{

// Abstract base for a recordable render pass. It owns no GPU objects — each pass manages its
// own pipeline/descriptors as members (a `string::gpu::pipeline` member, by convention). The base is just
// the interface plus the current screen size and the typed resource usages a future render
// graph will use to order passes and derive barriers.
struct Pass
{
    VkExtent2D screen_size{};

    // The resources this pass reads/writes and how (Access + stage). Reads and writes share
    // one list; is_write(usage.access) distinguishes them. Not yet consumed — the forward hook
    // the render graph will build execution order + barriers from.
    std::vector<ResourceUsage> usages;

    // Brief 11 Phase 2 (M3): per-pass enable/disable. When set and returning false, the renderer
    // skips this pass entirely this frame — it leaves the graph, the execution order, and the
    // written-resource set (so downstream reads see whatever the target already holds; there is no
    // graceful-degrade fallback in the current planner, so disabling a producer with hard consumers
    // is the caller's responsibility for now — safe for the top-level passes, which only write the
    // color target that composite reads). Empty predicate = always enabled (byte-identical default).
    // This is the pass-toggle mechanism the debug UI (brief 14) drives via CVars.
    std::function<bool()> enable_predicate;
    bool is_enabled() const { return !enable_predicate || enable_predicate(); }

    virtual ~Pass() = 0;
    // Human-readable identity for tooling (profiler zones, the future scene/draw inspector, log
    // lines). Defaults to "Pass" so existing passes need no change; passes SHOULD override it with
    // a stable literal (e.g. "geometry", "grid2d", "ui"). Returns a string_view into storage the
    // pass owns for its lifetime — return a string literal or a member, not a temporary.
    virtual std::string_view debug_name() const { return "Pass"; }
    // Per-frame CPU update. Defaults to a no-op so passes that don't need one (grid, most
    // static passes) can skip it; record() is the only method a pass must implement.
    virtual void update(float /*delta_time*/, uint16_t /*current_frame*/) {}
    // Optional compute prepass, recorded *outside* dynamic rendering before the graphics groups
    // (e.g. GPU culling that writes an indirect buffer the pass's record() then draws). Returns
    // true if it recorded any compute work, so the renderer knows to barrier compute->draw.
    virtual bool record_compute(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) { return false; }
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) = 0;
    // Virtual so a pass can (re)allocate viewport-sized resources at the DETERMINISTIC resize point
    // — init + window resize, both before any per-frame update() — instead of lazily in update()
    // where allocation can race a consumer's read (brief 16 M2: the froxel ring). Override MUST call
    // the base (or set screen_size) then do its screen-dependent work.
    virtual void resize(VkExtent2D extent) { screen_size = extent; }

    // Brief 11 endgame: the pass NATURE flags (compute-only / prepass-only / has-async) are NO LONGER
    // Pass virtuals — the app declares each pass's nature FLUENTLY when it authors the render graph
    // (FrameGraph PassSpec .computeOnly()/.prepass()/.async()). The Pass base carries only what every
    // pass genuinely provides: record(), the optional record_compute()/record_async_compute() bodies,
    // its live `usages`/`async_usages`, and the update()/resize()/bind_color_source() lifecycle.

    // Brief 09: the renderer re-binds the offscreen HDR color target into the bindless table at
    // init and on every resize, then notifies the passes that consume it (the post chain samples
    // and storage-writes it). `physical_id` is the allocator id of the CURRENT color attachment
    // (the logical COLOR_TARGET id in `usages` still names it for the graph); `sampled_slot` is
    // its bindless combined-image-sampler slot.
    virtual void bind_color_source(uint32_t /*sampled_slot*/,
                                   string::gpu::resource_id /*physical_id*/) {}

    // --- Brief 04e M4: async-compute lane work --------------------------------------------------
    // A DEPENDENCY-FREE compute chain (inputs host-written or persistent; nothing produced by
    // this frame's other GPU work) that the renderer places onto an async compute lane when the
    // capability table exposes one. On 1-lane hardware it records inline on the main queue —
    // same graph, serialized placement, zero special cases. `async_usages` declares what the
    // chain WRITES (on the async queue) and where the main-queue frame READS it; the renderer
    // derives the cross-lane timeline edge + queue-family ownership transfer (async placement)
    // or the plain tracker barriers (inline) from those declarations.
    std::vector<ResourceUsage> async_usages;
    // record_async_compute() is the async chain body (default no-op); the dynamic "has work this frame?"
    // gate is declared fluently via PassSpec.async() (no has_async_compute() virtual on the base).
    virtual void record_async_compute(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) {}

    // Brief 11 step 2: the 04d two-phase hooks (breaks_scene_group / record_between /
    // record_after_between) and depth_resolve_target (B1) are RETIRED. The two-phase occlusion path is
    // three ordinary registered passes now — geometry.phase1 (raster), hiz.build (compute_only, runs
    // standalone between the two MSAA groups), geometry.phase2 (raster into the reloaded MSAA group) —
    // scheduled purely from their declared usages + the Access::DepthResolve marker. No special hooks.
};

// A pure-virtual destructor still needs a definition so derived passes can link.
inline Pass::~Pass() = default;

} // namespace String