#pragma once

#include <cstdint>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string::render
{

// Push for the brief-07 IBL compute chain (matches Push in shaders/ibl.slang; std430: float4s at
// 0/16/32/48, scalars from 64, the 8-byte pointer 8-aligned at 96).
struct IblPush
{
    glm::vec4 sun_dir;       // xyz sun dir; w furnace flag (capture writes uniform white)
    glm::vec4 sky_zenith;
    glm::vec4 sky_ground;
    glm::vec4 sun_color;
    uint32_t src_slot;       // 64
    uint32_t dst_slot;       // 68
    uint32_t dst_size;       // 72
    uint32_t src_size;       // 76
    float roughness;         // 80  prefilter: PERCEPTUAL roughness of this ladder mip
    uint32_t sample_count;   // 84
    uint32_t mip_count;      // 88  capture chain mips (prefilter PDF lod clamp)
    uint32_t _pad0;          // 92
    VkDeviceAddress sh;      // 96  ShBuffer address (sh_project)
};
static_assert(offsetof(IblPush, src_slot) == 64);
static_assert(offsetof(IblPush, sh) == 96);
static_assert(sizeof(IblPush) == 104);

// The shared sun/sky lighting the IBL chain captures (owned by the scene, handed over in tick()).
struct IblLighting
{
    glm::vec3 sun_dir;
    glm::vec3 sky_zenith;
    glm::vec3 sky_ground;
    glm::vec3 sun_color;
    float sun_intensity;
    bool furnace;
};

// Brief 07 dynamic sky IBL: the live sky is captured to a small cubemap, GGX-prefiltered into a
// roughness ladder and SH-projected — all on the GPU, re-run only when the sun moves past a
// threshold (amortized). Products: the prefiltered SamplerCube ladder, the SH buffer (9 float4 E/pi
// coefficients, also read by probe GI) and the split-sum DFG LUT (baked once).
//
// Brief 20: the chain is FOURTEEN declared compute passes, not one hook with nine hand-rolled
// barriers. The cubemaps / DFG LUT / SH buffer are graph resources the app declares and hands in;
// this object owns only its five pipelines and the amortization state. Every edge that used to be a
// hand barrier is now a declared slice:
//   * the capture mip chain is one pass per mip, reading mip m-1 and writing mip m
//   * the prefilter ladder is one pass per mip, each reading the whole cube and writing its own mip,
//     so the six writes stay disjoint and nothing serialises them (as today, deliberately)
//   * first-write discard, the cross-frame WAR against last frame's shading reads, and the read edge
//     into the lit fragments all derive from the declarations plus tracked state
class ibl_component
{
public:
    // Sizes the app needs to declare the graph resources with (see the class comment).
    static constexpr uint32_t kEnvSize = 128;           // capture / prefilter face size
    static constexpr uint32_t kEnvCaptureMips = 6;      // capture average chain (PDF mips + SH source)
    static constexpr uint32_t kEnvPrefilterMips = 6;    // roughness ladder 128..4 (r = mip/(mips-1))
    static constexpr uint32_t kShSourceMip = 3;         // 16x16 capture mip the SH projects from
    static constexpr uint32_t kDfgSize = 128;
    static constexpr uint32_t kShCoefficients = 9;      // L2, float4 each

    // Builds the five ibl.slang compute pipelines. It allocates nothing: every image and buffer the
    // chain touches is declared to the graph by the app and resolved through pass_context.
    explicit ibl_component(string::engine_context& ctx);
    ~ibl_component();

    ibl_component(const ibl_component&) = delete;
    ibl_component& operator=(const ibl_component&) = delete;

    // Author the chain onto the graph. Called once, at startup, before the first tick().
    //   env_capture      cube, kEnvCaptureMips  — written mip by mip, sampled by the prefilter
    //   env_prefiltered  cube, kEnvPrefilterMips — the roughness ladder the shading samples
    //   dfg_lut          2D, baked once
    //   sh               9 x float4, written by the projection, read by every lit fragment + probe GI
    void declare(::string::frame_graph& fg,
                 ::string::gpu::image env_capture, ::string::gpu::image env_prefiltered,
                 ::string::gpu::image dfg_lut, ::string::gpu::buffer sh);

    // Ordinary app code: decide whether the chain re-runs this frame (sun moved past cos(0.1 deg),
    // furnace flip, first frame, or forced) and latch the lighting its dispatches will push. The
    // decision is made HERE, once, and every declared pass's conditional reads the same latch — a
    // pass that cleared it while recording would switch the chain off halfway through itself.
    void tick(const IblLighting& light, bool force_every_frame);

    // Does the chain run this frame? (The latch tick() computed.)
    bool needs_update() const { return chain_this_frame_; }
    bool primed() const { return ibl_primed_; }
    uint64_t update_count() const { return ibl_update_count_; }
    static constexpr uint32_t env_mips() { return kEnvPrefilterMips; }

private:
    void dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                  const IblPush& push, uint32_t gx, uint32_t gy, uint32_t gz) const;
    IblPush base_push() const;

    void record_dfg(::string::pass_context& ctx);
    void record_capture(::string::pass_context& ctx);
    void record_capture_mip(::string::pass_context& ctx, uint32_t mip);
    void record_sh_project(::string::pass_context& ctx);
    void record_prefilter(::string::pass_context& ctx, uint32_t mip);

    ::string::gpu::device& device_;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // The declared handles, latched by declare(). Logical, never physical: the records resolve them
    // (and their per-mip slices' bindless slots) through pass_context.
    ::string::gpu::image env_capture_{};
    ::string::gpu::image env_prefiltered_{};
    ::string::gpu::image dfg_lut_{};
    ::string::gpu::buffer sh_buffer_{};

    ::string::gpu::shader_program* env_capture_program_ = nullptr;
    ::string::gpu::shader_program* env_mip_program_ = nullptr;
    ::string::gpu::shader_program* env_prefilter_program_ = nullptr;
    ::string::gpu::shader_program* sh_project_program_ = nullptr;
    ::string::gpu::shader_program* dfg_program_ = nullptr;

    static constexpr uint32_t kPrefilterSamples = 64;
    static constexpr uint32_t kDfgSamples = 1024;

    bool dfg_baked_ = false;                 // one-time bake; also the DFG pass's conditional
    bool ibl_primed_ = false;                // at least one capture recorded
    bool ibl_update_pending_ = false;        // trigger seen -> the next tick() runs the chain
    bool chain_this_frame_ = false;          // this frame's latched decision
    IblLighting light_{};                    // this frame's latched lighting
    glm::vec3 ibl_captured_sun_dir_{ 0.0f };
    bool ibl_captured_furnace_ = false;
    uint64_t ibl_update_count_ = 0;          // instrumentation (amortization honesty)
};

}  // namespace string::render
