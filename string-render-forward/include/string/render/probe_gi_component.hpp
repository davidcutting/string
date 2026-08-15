#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/render/scene_bridge.hpp>
#include <string/render/lighting_data.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/render/probe_gi.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string
{
class composite_pass;
}

namespace string::render
{

// Brief 09b: RELIGHTABLE PROBE GI. A uniform probe grid fit to the scene AABB. A static per-probe
// capture G-buffer (albedo, normal, distance) + visibility (mean/mean^2 distance) octahedral atlas
// pair, rasterized once at load; a compute pass RELIGHTS them into an octahedral irradiance atlas
// whenever the sun moves (like the sky IBL). Shading samples the irradiance atlas
// (Chebyshev-visibility-gated 8-probe trilinear) for the ambient DIFFUSE term, falling back to the
// sky-SH ambient outside the volume. See probe_gi.hpp / probe_common.slang for the layout + math.
//
// Brief 20 — a plain app-owned object. No base class, no owned images or buffers, no bindless slot
// table, no sampler. What it owns is the five pipelines and the amortization state; every atlas,
// cube and buffer is a GRAPH resource the application declares and hands to declare(), and every
// slot and address a push carries is resolved from this pass's own declaration while it records.
//
// The nine hand-rolled barriers are gone. The chain is SEVEN declared passes:
//
//   gi.clear                  one-shot init: zero the irradiance atlas, prime the capture/visibility
//                             atlases (clear_main), default the activation + relocation buffers, and
//                             upload the flat capture table
//   gi.capture.{0..K-1}       one probe each: a 6-face MULTIVIEW raster of the coarse-LOD scene into
//                             the shared cube G-buffer (all six faces in ONE pass, viewMask 0x3F)
//   gi.collapse.{0..K-1}      the matching collapse: reads the cube as a SamplerCube, writes THIS
//                             probe's tile of the octahedral atlases, classifies + relocates it
//   gi.relight                the amortized hysteresis relight of 128 probes/frame
//   gi.debug                  the instanced probe-debug spheres, into the scene attachments
//
// The capture/collapse pair is declared ONCE PER PROBE SLOT (kProbesPerFrame of them) rather than
// looped inside one pass, because the cube G-buffer is destructively reused between probes: the
// per-probe sequencing IS a chain of WAR/WAW edges over one resource, which is exactly what the
// graph derives once the stages are passes. The unrolling is what preserves the algorithm.
//
// Two facts about granularity, stated so nobody tries to slice them:
//   * the per-probe unit inside an atlas is a TILE, which Vulkan cannot express as a subresource, so
//     the atlases are declared whole-image;
//   * there is no per-FACE sequencing anywhere — capture writes all six faces in one multiview pass
//     and collapse reads all six through a SamplerCube — so per-layer declarations buy nothing.
// Both are declared honestly and neither changes a barrier.
//
// It still reads the scene tables, sun/sky, the meshlet heaps and the frame's SceneData through
// GeometryScene; the sky-SH buffer, the SceneData ring and the CSM cascades come in as declared
// handles, because their EDGES are what the deleted barriers used to assert by hand.
class probe_gi_component
{
public:
    // Cube face size (cheap; > the vis tile so no aliasing). All six faces render in ONE multiview
    // pass, so the amortization unit is PROBES/frame, not faces.
    static constexpr uint32_t kProbeCubeFace = 32;
    static constexpr uint32_t kProbeCubeViewMask = 0x3Fu;   // 6-face multiview mask
    // Probes captured per frame. Each is one multiview draw over the coarse-LOD meshlet domain
    // (distance + face culled on the GPU), so this bounds the per-frame bake burst; a ~750-probe
    // bake still finishes in a few seconds. This is ALSO the number of capture/collapse pass pairs
    // declared on the graph — see the class comment.
    static constexpr uint32_t kProbesPerFrame = 2;
    // M2 amortized relight: K probes per frame, round-robin. A sun trigger arms N converge passes;
    // each full wrap propagates the bounce one step further (infinite bounce) and the hysteresis EMA
    // settles (h^N residual), then relight goes idle until the sun moves (~0 static cost).
    static constexpr uint32_t kRelightProbesPerFrame = 128;
    static constexpr uint32_t kRelightConvergePasses = 32;

    // The graph resources the application declares and hands over. Sizes are derived from the probe
    // grid — see fit_volume(), which is the SAME fit this component uses, so the app sizes its
    // declarations from the identical numbers rather than a duplicate rule.
    //
    //   irradiance      RGBA16F, tile_grid * kProbeIrradStride. Relit in place (read + write).
    //   capture_gbuf    RGBA16F, tile_grid * kProbeVisStride. Static: world normal.xyz + hit dist(w).
    //   capture_albedo  RGBA16F, tile_grid * kProbeVisStride. Static: captured base colour.
    //   visibility      RGBA16F, tile_grid * kProbeVisStride. Static: mean / mean^2 distance.
    //   cube_albedo     cube RGBA16F kProbeCubeFace^2, colour attachment + sampled.
    //   cube_nd         cube RGBA16F kProbeCubeFace^2, colour attachment + sampled.
    //   cube_depth      cube D32, depth attachment only.
    //   active          uint per probe: 1 active / 0 inside geometry.
    //   offset          float4 per probe: xyz world relocation offset.
    //   meshlet_table   uvec2 per coarse-LOD meshlet: {draw index, global meshlet id}.
    struct resources
    {
        ::string::gpu::image irradiance{};
        ::string::gpu::image capture_gbuf{};
        ::string::gpu::image capture_albedo{};
        ::string::gpu::image visibility{};
        ::string::gpu::image cube_albedo{};
        ::string::gpu::image cube_nd{};
        ::string::gpu::image cube_depth{};
        ::string::gpu::buffer active{};
        ::string::gpu::buffer offset{};
        ::string::gpu::buffer meshlet_table{};
    };

    // Builds the five programs and the flat capture table's staging copy. It allocates no atlas, no
    // cube, no view, no sampler and no bindless slot. `scene_samples` is the MSAA sample count of the
    // scene attachments the debug spheres draw into (engine_context no longer supplies one: the app
    // declares the scene attachments, so the app is what chooses the sample count).
    probe_gi_component(string::engine_context& ctx, const scene_bridge* bridge,
                       const ibl_component* ibl, const string::composite_pass* composite,
                       VkSampleCountFlagBits scene_samples);
    ~probe_gi_component();

    probe_gi_component(const probe_gi_component&) = delete;
    probe_gi_component& operator=(const probe_gi_component&) = delete;

    // The probe grid fit for a scene AABB, at the current r.gi.spacing. A pure function so the
    // application can size its atlas declarations from exactly the grid this component captures into.
    static ProbeVolume fit_volume(glm::vec3 aabb_min, glm::vec3 aabb_max);
    // The flat capture dispatch domain: {row index, global meshlet id} per COARSEST-LOD meshlet,
    // built once from the bridge's DrawInfo rows. Pure, so the app can size the table buffer.
    static std::vector<glm::uvec2> capture_table(std::span<const GpuDrawInfo> draws);

    // Author the chain onto the graph. Called once, at startup, before the first tick().
    //   res          the graph resources above
    //   sky_sh       the IBL's L2 sky-SH buffer (relight's miss/fallback source)
    //   scene_data   this frame's SceneData ring (relight's 1-tap CSM sun shadow reads the cascades
    //                through it)
    //   cascades     the CSM cascade images. Declared as READS so the write-after-read edge against
    //                the shadow passes derives — that is one half of the deleted :739 barrier, and it
    //                requires this component to be authored BEFORE shadow_maps (the graph's edges run
    //                earlier-authored -> later-authored, so authoring order is what makes it a WAR).
    //   scene_color/scene_depth  the scene attachments the debug spheres draw into.
    void declare(::string::frame_graph& fg, const resources& res,
                 ::string::gpu::buffer sky_sh, ::string::gpu::buffer scene_data,
                 std::span<const ::string::gpu::image> cascades,
                 ::string::gpu::image scene_color, ::string::gpu::image scene_depth);

    // Ordinary per-frame CPU work the app calls before the graph executes. It makes EVERY decision
    // this frame's passes act on, once: the debug-sphere mode, the sun re-arm, which probes the
    // capture slots take, and whether relight runs and over which slice. A pass body that advanced
    // one of these cursors while recording would gate the rest of itself off — so the bookkeeping
    // lives with the decision, here.
    void tick();

    // Fill this frame's probe fields into SceneData: the shading-side gate, the volume, and the
    // atlas slots + buffer addresses. Called by the CONSUMER (the lit pass) from inside ITS recording
    // callback, with its own context, because the bindless slots and the addresses belong to that
    // pass's declaration. The caller must therefore declare reads of the irradiance and visibility
    // atlases and of the activation + relocation buffers.
    void fill_scene_data(SceneData& scene, ::string::pass_context& ctx) const;

    bool volume_valid() const { return probe_volume_.valid; }
    const ProbeVolume& volume() const { return probe_volume_; }

private:
    // --- pass bodies (each resolves everything through the context) --------------------------------
    void record_clear(::string::pass_context& ctx);
    void record_capture(::string::pass_context& ctx, uint32_t slot);
    void record_collapse(::string::pass_context& ctx, uint32_t slot);
    void record_relight(::string::pass_context& ctx);
    void record_debug(::string::pass_context& ctx);

    // Is capture slot `slot` doing a probe this frame? (Latched by tick(); the passes' conditional.)
    bool capture_slot_runs(uint32_t slot) const { return slot < capture_count_; }
    // The (probe, bake round) this frame's capture slot `slot` works on.
    uint32_t capture_work(uint32_t slot) const { return capture_base_ + slot; }

    ::string::gpu::device* device_ = nullptr;
    ::string::gpu::resource_allocator* allocator_ = nullptr;   // scene meshlet heaps + the table staging
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    const scene_bridge* bridge_ = nullptr;
    const ibl_component* ibl_ = nullptr;
    // Exposure source for the probe debug view (ctor-injected; never null).
    const string::composite_pass* composite_ = nullptr;

    // The declared handles, latched by declare(). Logical, never physical.
    resources res_{};
    ::string::gpu::buffer sky_sh_{};
    ::string::gpu::buffer scene_data_{};

    ProbeVolume probe_volume_{};
    float probe_cull_far_ = 0.0f;         // capture distance-cull radius (bounded ~= a few cells)
    uint32_t probe_total_meshlets_ = 0;   // coarse-LOD meshlets (flat capture dispatch domain)

    ::string::gpu::shader_program* clear_program_ = nullptr;      // atlas init (clear_main)
    ::string::gpu::shader_program* capture_program_ = nullptr;    // cube-face G-buffer raster
    ::string::gpu::shader_program* collapse_program_ = nullptr;   // cube -> octa atlases + classify
    ::string::gpu::shader_program* relight_program_ = nullptr;    // relight + convolve
    ::string::gpu::shader_program* debug_program_ = nullptr;      // instanced debug spheres

    // The capture table's host copy, uploaded once by gi.clear. Built at construction (the table is a
    // pure function of the meshlet model) and kept until the dtor: it is ~100 KB and freeing it would
    // need to know when the upload retired, which is a frame-pacing question this has no business
    // answering.
    ::string::gpu::resource_id table_staging_ = 0;
    VkDeviceSize table_bytes_ = 0;

    // --- amortization state (all latched/advanced in tick()) --------------------------------------
    bool init_done_ = false;              // gi.clear has recorded
    uint32_t capture_cursor_ = 0;         // next capture work unit (probe + round * total)
    uint32_t capture_base_ = 0;           // this frame's first work unit
    uint32_t capture_count_ = 0;          // capture slots running this frame (0..kProbesPerFrame)
    bool captured_ = false;               // the full two-round bake has been recorded
    double capture_ms_ = 0.0;             // accumulated capture wall time (logged on completion)

    bool primed_ = false;                 // at least one FULL relight pass has completed
    bool relight_pending_ = false;        // armed passes remain
    bool relight_runs_ = false;           // this frame's latched decision (gi.relight's conditional)
    bool relight_first_ = true;           // the primed state THIS frame's dispatch must push
    uint32_t relight_cursor_ = 0;         // next probe to relight within the current pass
    uint32_t relight_base_ = 0;           // this frame's slice
    uint32_t relight_count_ = 0;
    uint32_t relight_passes_left_ = 0;    // full passes remaining before relight goes idle
    glm::vec3 relit_sun_dir_{ 0.0f };

    uint32_t debug_mode_ = 0;             // r.gi.probe_debug snapshot for this frame's draw
};

}  // namespace string::render
