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
    // Builds the depth-only meshlet program. The per-cascade work lists are graph transients the app
    // declares and hands to declare() — this owns the shadow RENDER, not any allocation.
    shadow_maps(string::engine_context& ctx, GeometryScene* scene);
    ~shadow_maps();

    shadow_maps(const shadow_maps&) = delete;
    shadow_maps& operator=(const shadow_maps&) = delete;

    // Author onto the graph: ONE pass per cascade, each a depth-only render into its own cascade
    // image plus the indirect work list the meshlet culler filled earlier this frame. `cascades` is
    // the application's declared cascade images, in cascade order; `worklists` carries the per-cascade
    // list buffers the culler writes and these draws consume. `scene_data`/`joint_palette` (brief 23):
    // skinned casters pull the skin stream + this slot's palettes through SceneData at the MESH
    // stage — the cascades must deform with the body, not shadow the bind pose.
    void declare(::string::frame_graph& fg, std::span<const ::string::gpu::image> cascades,
                 const WorklistSet& worklists, ::string::gpu::buffer scene_data,
                 ::string::gpu::buffer joint_palette = {});

    uint32_t cascade_count() const { return scene_ != nullptr ? scene_->settings_.cascade_count : 0u; }

private:
    // Is there anything to draw? Folded into the graph conditional, so a scene with no resident
    // geometry SKIPS the cascade passes and every consumer transparently reads the declared neutral
    // (unshadowed) fallback instead.
    bool ready() const;

    void record_cascade(::string::pass_context& ctx, uint32_t cascade, ::string::gpu::buffer list);

    ::string::gpu::device* device_ = nullptr;
    ::string::gpu::resource_allocator* allocator_ = nullptr;
    ::string::gpu::descriptor_table* descriptors_ = nullptr;
    GeometryScene* scene_ = nullptr;
    ::string::gpu::shader_program* program_ = nullptr;
    // Brief 23: SceneData's graph handle, resolved per cascade record for the push (the skin
    // stream + palette pointers live in it).
    ::string::gpu::buffer scene_data_{};

};

}  // namespace string::render
