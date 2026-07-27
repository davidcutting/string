#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace sandbox
{

// Push for the brief-07 IBL compute chain (matches Push in shaders/ibl.slang; std430: float4s at
// 0/16/32/48, scalars from 64, the 8-byte pointer 8-aligned at 96). Moved out of geometry_pass.hpp
// with the IblComponent extraction (brief 11).
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

// The shared sun/sky lighting the IBL chain captures (owned by GeometryPass, passed in per update).
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
// threshold (amortized). Products: env_prefiltered_ (SamplerCube ladder), sh_buffer_ (9 float4
// E/pi coefficients, also read by probe GI), dfg_lut_ (split-sum BRDF LUT, baked once). Owns all of
// these + its five compute pipelines; the shading-facing slots + SH address feed SceneData via
// accessors. GeometryPass Phase-1 component (brief 11).
class IblComponent
{
    string::gpu::device* device_ = nullptr;
    string::gpu::resource_allocator* allocator_ = nullptr;
    string::gpu::descriptor_table* table_ = nullptr;

    static constexpr uint32_t kEnvSize = 128;           // capture / prefilter face size
    static constexpr uint32_t kEnvCaptureMips = 6;      // capture average chain (PDF mips + SH source)
    static constexpr uint32_t kEnvPrefilterMips = 6;    // roughness ladder 128..4 (r = mip/(mips-1))
    static constexpr uint32_t kShSourceMip = 3;         // 16x16 capture mip the SH projects from
    static constexpr uint32_t kDfgSize = 128;
    static constexpr uint32_t kPrefilterSamples = 64;
    static constexpr uint32_t kDfgSamples = 1024;

    string::gpu::resource_id env_capture_ = 0;          // cube, RGBA16F, kEnvCaptureMips
    string::gpu::resource_id env_prefiltered_ = 0;      // cube, RGBA16F, kEnvPrefilterMips
    string::gpu::resource_id dfg_lut_ = 0;              // 2D RGBA16F (rg used)
    string::gpu::resource_id sh_buffer_ = 0;            // 9 x float4, device-local
    uint32_t env_capture_sample_slot_ = 0;              // SamplerCube (prefilter source)
    uint32_t env_prefiltered_slot_ = 0;                 // SamplerCube (shading)
    uint32_t dfg_sample_slot_ = 0;                      // Sampler2D (shading)
    uint32_t dfg_storage_slot_ = 0;
    std::vector<VkImageView> env_capture_mip_views_;    // per-mip 2D_ARRAY storage views
    std::vector<uint32_t> env_capture_mip_slots_;
    std::vector<VkImageView> env_prefiltered_mip_views_;
    std::vector<uint32_t> env_prefiltered_mip_slots_;
    VkSampler env_sampler_ = VK_NULL_HANDLE;            // linear, clamp, mip-linear (env + LUT)
    string::gpu::shader_program* env_capture_program_ = nullptr;
    string::gpu::shader_program* env_mip_program_ = nullptr;
    string::gpu::shader_program* env_prefilter_program_ = nullptr;
    string::gpu::shader_program* sh_project_program_ = nullptr;
    string::gpu::shader_program* dfg_program_ = nullptr;
    bool dfg_baked_ = false;
    bool ibl_layouts_initialized_ = false;   // env images moved UNDEFINED -> GENERAL once
    bool ibl_primed_ = false;                // at least one capture recorded
    bool ibl_update_pending_ = false;        // begin_frame trigger -> record_update runs the chain
    glm::vec3 ibl_captured_sun_dir_{ 0.0f };
    bool ibl_captured_furnace_ = false;
    uint64_t ibl_update_count_ = 0;          // instrumentation (amortization honesty)

public:
    // Create the cubemaps / SH buffer / DFG LUT + the five ibl.slang compute pipelines.
    void init(String::engine_context& context);
    // Decide whether the chain must re-run this frame (sun moved past cos(0.1 deg), furnace flip,
    // first frame, or forced). Sets the internal pending flag consumed by needs_update().
    void begin_frame(const glm::vec3& sun_dir, bool furnace, bool force_every_frame);
    bool needs_update() const
    {
        return (ibl_update_pending_ || !dfg_baked_) && env_capture_program_ != nullptr && env_capture_ != 0;
    }
    // Record the full capture -> mip -> SH + prefilter chain (one frame; DFG bake rides the first
    // call). Clears the pending flag and latches primed/captured state.
    void record_update(string::gpu::command_recorder& recorder, const IblLighting& light);
    // dbg.ibl_verify: read the DFG LUT + SH back and check against CPU references. Stalls; debug only.
    void run_verification(bool furnace);
    // Tear down the pipelines + resources (GeometryPass destructor calls this).
    void destroy();

    bool primed() const { return ibl_primed_; }
    bool update_pending() const { return ibl_update_pending_; }
    uint64_t update_count() const { return ibl_update_count_; }
    string::gpu::resource_id sh_buffer() const { return sh_buffer_; }
    VkDeviceAddress sh_address() const;
    uint32_t env_slot() const { return env_prefiltered_slot_; }
    uint32_t env_mips() const { return kEnvPrefilterMips; }
    uint32_t dfg_slot() const { return dfg_sample_slot_; }
};

}  // namespace sandbox
