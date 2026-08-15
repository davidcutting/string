#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <string/gpu/pass_context.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/render/lighting_data.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/render/scene_bridge.hpp>

namespace string
{
class composite_pass;
}

namespace string::render
{

class froxel_component;
class ibl_component;
class gtao_chain;
class probe_gi_component;
class shadow_maps;

// The `scene.upload` pass: fills SceneData (the per-frame GPU scene constants) + the light ring,
// and reads back the GPU culling stats. Moved out of geometry_pass with the GeometryScene
// deletion — authoring the scene constants was never the meshlet technique's job.
//
// Every input arrives EXPLICITLY: the bridge's frame snapshot plus constructor-injected component
// pointers. This replaces the GeometryScene back-pointer bus whose two never-assigned entries
// (froxel, ibl) silently killed probe GI and zeroed the froxel grid — with constructor wiring,
// not passing a component is a visible decision at the wiring site, not a forgotten side effect.
class scene_uniforms
{
public:
    scene_uniforms(engine_context& ctx, const scene_bridge& bridge, const froxel_component* froxel,
                   const ibl_component* ibl, const gtao_chain* gtao, probe_gi_component* gi,
                   const shadow_maps* shadow, string::composite_pass* composite);

    // Author `scene.upload` onto the graph (the same declaration geometry_pass used to make, in
    // the same authoring position — the app calls this immediately before geometry declares).
    void declare(::string::frame_graph& fg, ::string::gpu::buffer scene_data,
                 ::string::gpu::buffer lights, ::string::gpu::buffer stats,
                 std::span<const ::string::gpu::image> cascades, ::string::gpu::image gtao_ao,
                 ::string::gpu::image env_prefiltered, ::string::gpu::image dfg_lut,
                 ::string::gpu::buffer ibl_sh, ::string::gpu::buffer froxels,
                 ::string::gpu::buffer joint_palette);

    // Per-frame CPU work (the furnace exposure pin — scene policy that must not live in a
    // technique). Call before the graph executes.
    void tick();

    // Latest GPU culling stats (read back one frame late, at record time — only pass_context can
    // resolve the ring slot's mapped pointer).
    const GpuMeshStats& stats_latest() const { return stats_latest_; }

private:
    void record(::string::pass_context& ctx);

    const scene_bridge& bridge_;
    const froxel_component* froxel_ = nullptr;
    const ibl_component* ibl_ = nullptr;
    const gtao_chain* gtao_ = nullptr;
    probe_gi_component* gi_ = nullptr;
    const shadow_maps* shadow_ = nullptr;
    // The display chain: tick() pins its exposure for the furnace gate, record() mirrors its
    // scale into SceneData for display-referred debug views (ctor-injected; never null).
    string::composite_pass* composite_ = nullptr;
    uint16_t frames_in_flight_ = 0;

    ::string::gpu::buffer scene_data_{};
    ::string::gpu::buffer lights_buffer_{};
    ::string::gpu::buffer stats_{};
    std::array<::string::gpu::image, kMaxCascades> cascades_{};
    uint32_t cascade_count_ = 0;
    ::string::gpu::image gtao_ao_{};
    ::string::gpu::image env_prefiltered_{};
    ::string::gpu::image dfg_lut_{};
    ::string::gpu::buffer ibl_sh_{};
    ::string::gpu::buffer froxels_{};
    ::string::gpu::buffer joint_palette_{};

    GpuMeshStats stats_latest_{};
};

}  // namespace string::render
