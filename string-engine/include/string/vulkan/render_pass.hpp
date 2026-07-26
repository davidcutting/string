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
    void resize(VkExtent2D extent) { screen_size = extent; }

    // --- Brief 09: compute-only frame passes -----------------------------------------------------
    // A pass that records NO draws: its record() runs OUTSIDE any rendering group, at its toposorted
    // position in the frame (post-processing between the scene resolve and the composite). The
    // renderer derives its image transitions (StorageImageRead/Write -> GENERAL, SampledRead) and
    // buffer barriers from `usages` at that point — same contract as attachment passes, different
    // execution site. Its record_compute() is NOT called in the frame-top compute prepass (the
    // whole pass already runs at a compute point).
    virtual bool compute_only() const { return false; }

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
    virtual bool has_async_compute() const { return false; }
    virtual void record_async_compute(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) {}

    // --- Brief 04d: two-phase occlusion support -------------------------------------------------
    // A pass that must interleave a COMPUTE step between two sets of draws into the SAME MSAA scene
    // targets (phase-1 opaque -> build HiZ pyramid -> phase-2 opaque) cannot dispatch that compute
    // inside the renderer's single scene render pass. These hooks let the renderer break the scene
    // render group after this pass, run the pass's compute step OUTSIDE rendering, then reopen the
    // group PRESERVING (LOAD, not CLEAR) the MSAA color+depth this pass already wrote.
    //
    // breaks_scene_group(): true -> the renderer ends the scene render pass after this pass's
    //   record(), calls record_between(), and the following scene group loads (does not clear) the
    //   MSAA targets and defers the MSAA-color resolve to the LAST group.
    virtual bool breaks_scene_group() const { return false; }
    // record_between(): recorded OUTSIDE dynamic rendering, after the pre-break group's EndRendering
    // and before the post-break group's BeginRendering. Compute + image barriers are legal here.
    virtual void record_between(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) {}
    // record_after_between(): recorded INSIDE the reopened (post-break) scene render group, before that
    // group's own passes. This is where the breaker draws its phase-2 geometry into the reloaded MSAA
    // targets (tested against the pyramid record_between just built). record() drew phase-1.
    virtual void record_after_between(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) {}
    // Brief 11 P2 (B1): depth_resolve_target() is RETIRED — the group's MSAA-depth resolve target is
    // now named by a declared Access::DepthResolve usage the renderer scans for (resource_usage.hpp),
    // not a per-pass virtual hook. (breaks_scene_group/record_between/record_after_between follow in B2.)
};

// A pure-virtual destructor still needs a definition so derived passes can link.
inline Pass::~Pass() = default;

} // namespace String