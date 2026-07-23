#pragma once

#include <string/gpu/command_recorder.hpp>
#include <string/vulkan/resource_usage.hpp>

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
    // depth_resolve_target(): when this pass breaks the scene group, the renderer resolves the MSAA
    // depth into this single-sample image with VK_RESOLVE_MODE_MIN_BIT at the pre-break group's
    // EndRendering (reverse-Z: min = farthest = conservative HiZ occluder). 0 = no depth resolve.
    // The pass owns the image (single-sample, DEPTH usage + SAMPLED, D32); record_between() then
    // builds its HiZ pyramid from it. Frame index selects the per-frame-in-flight target.
    virtual string::gpu::resource_id depth_resolve_target(uint16_t /*current_frame*/) const { return 0; }
};

// A pure-virtual destructor still needs a definition so derived passes can link.
inline Pass::~Pass() = default;

} // namespace String