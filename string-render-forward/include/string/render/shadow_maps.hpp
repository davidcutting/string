#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/render/geometry/geometry_scene.hpp>
#include <string/render/lighting_data.hpp>

namespace string::render
{

// The cascaded shadow-map technique. Brief 20: a plain app-owned object — no base class, no owned
// render targets, no bindless slot table, no sampler.
//
// What it owns now is the DRAW: the depth-only meshlet program and the per-cascade work lists it
// reserves from the frame scratch arena. The cascade IMAGES are graph resources the application
// declares (one D32 image per cascade, sampler configured on the declaration) and hands to declare();
// their layouts, their clear, and the barriers against every consumer are derived from that.
//
// The per-frame-in-flight RING IS GONE. Every consumer of a cascade indexed the CURRENT frame slot
// and nothing ever read a previous one, so the ring only ever avoided frame N+1's depth write racing
// frame N's sample — a write-after-read edge the graph derives now that tracked state carries across
// the frame boundary. One image per cascade instead of frames_in_flight x cascades: 144 -> 48 MB.
//
// What deliberately stays outside: the cascade FIT (cascade_view_proj_ / splits / cull spheres) is
// camera-derived state in GeometryScene, read by the draw-cull compute and the lit shader as much as
// by this render; and the cull/expand computes, which are the shared meshlet culler operating on
// whichever list it is handed. So this owns the shadow RENDER, not a private copy of the culling
// machinery.
class shadow_maps
{
public:
    // Builds the depth-only meshlet program and reserves the per-cascade work lists from the frame
    // scratch arena. The reservation must happen at construction, before the renderer materializes
    // the arena (FrameScratch::reserve's contract).
    shadow_maps(String::engine_context& ctx, GeometryScene* scene);
    ~shadow_maps();

    shadow_maps(const shadow_maps&) = delete;
    shadow_maps& operator=(const shadow_maps&) = delete;

    // Author onto the graph: ONE pass per cascade, each a depth-only render into its own cascade
    // image plus the indirect work list the meshlet culler filled earlier this frame. `cascades` is
    // the application's declared cascade images, in cascade order; `worklists` is the frame scratch
    // buffer the culler writes and these draws consume.
    void declare(::string::frame_graph& fg, std::span<const ::string::gpu::image> cascades,
                 ::string::gpu::buffer worklists);

    // Ordinary per-frame CPU work the app calls before the graph executes: binds this run's work
    // lists to the scratch slot buffers once the renderer has materialized the arena. Idempotent.
    void tick();

    uint32_t cascade_count() const { return scene_ != nullptr ? scene_->settings_.cascade_count : 0u; }

    // The list the culler fills for `cascade` this frame. The culler writes lists it does not own,
    // so the offsets stay published here.
    const Worklist& worklist(uint16_t frame, uint32_t cascade) const { return lists_[frame][cascade]; }

private:
    // Is there anything to draw? Folded into the graph conditional, so a scene with no resident
    // geometry SKIPS the cascade passes and every consumer transparently reads the declared neutral
    // (unshadowed) fallback instead.
    bool ready() const;

    void record_cascade(::string::pass_context& ctx, uint32_t cascade,
                        ::string::gpu::buffer worklists);

    ::string::gpu::device* device_ = nullptr;
    ::string::gpu::resource_allocator* allocator_ = nullptr;
    ::string::gpu::descriptor_table* descriptors_ = nullptr;
    GeometryScene* scene_ = nullptr;
    ::string::gpu::shader_program* program_ = nullptr;
    uint32_t frames_in_flight_ = 0;

    std::vector<std::array<Worklist, kMaxCascades>> lists_;
    bool lists_bound_ = false;
};

}  // namespace string::render
