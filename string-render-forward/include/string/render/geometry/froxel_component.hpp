#pragma once

#include <cstdint>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string::render
{

// Push constant for the froxel light-binning compute (matches Push in shaders/froxel_cull.slang).
struct FroxelPush
{
    glm::mat4 view;
    glm::mat4 inv_proj;
    glm::uvec2 screen;
    glm::uvec2 grid;
    uint32_t slices;
    uint32_t tile_size;
    float near_plane;
    float far_plane;
    uint32_t light_count;
    uint32_t max_per_froxel;
    uint32_t _pad0;
    uint32_t _pad1;
    VkDeviceAddress lights;
    VkDeviceAddress froxels;
};

// The per-frame CPU inputs the binning needs. The screen extent and both device addresses are NOT
// here: the extent comes from pass_context, and the light + froxel buffers are declared handles the
// context resolves while the pass records — which is what makes a stale address structurally
// impossible rather than a matter of who updated whom first.
struct FroxelParams
{
    glm::mat4 view;
    glm::mat4 inv_proj;
    float near_plane;
    float far_plane;
    uint32_t light_count;
};

// Forward+ froxel light-binning: a dependency-free compute pass that bins the scene's local lights
// into the per-froxel index lists the lit fragment reads. Owns its compute pipeline and nothing else
// — the index buffer is a viewport-derived graph resource the app declares (see bytes_per_tile()).
//
// Brief 20: a plain app-owned object, no base class. It runs on the async lane (.async()); on 1-lane
// hardware the graph records it inline. No usages vector, no barriers, no ensure_capacity/resize —
// the buffer is sized from the declared viewport relationship and the graph reallocates it.
class froxel_component
{
public:
    explicit froxel_component(string::engine_context& ctx);
    ~froxel_component();

    froxel_component(const froxel_component&) = delete;
    froxel_component& operator=(const froxel_component&) = delete;

    // Author onto the graph: reads the local-light buffer, writes the froxel index buffer, on the
    // async lane. `froxels` is the tile-derived transient described by bytes_per_tile().
    void declare(::string::frame_graph& fg, ::string::gpu::buffer froxels,
                 ::string::gpu::buffer lights);

    // Ordinary app code: latch the camera + light count this frame's dispatch will push.
    void tick(const FroxelParams& params);

    // The dynamic gate the declared pass toggles on. False only when the Slang pipeline failed to
    // build; the dispatch otherwise always runs, because it is what ZEROES the per-froxel counts —
    // skipping it on a light-less frame would leave the previous frame's lists live.
    bool has_work() const { return program_ != nullptr; }

    // --- the froxel grid, as a pure function of the viewport ---------------------------------------
    // These are what the app's transient_buffer_info declares its size relationship with:
    //   .tile_pixels = kFroxelTileSize, .bytes_per_tile = froxel_component::bytes_per_tile()
    static uint32_t tiles_x(VkExtent2D screen);
    static uint32_t tiles_y(VkExtent2D screen);
    static uint32_t froxel_count(VkExtent2D screen);
    static VkDeviceSize bytes_per_tile();

private:
    void record(::string::pass_context& ctx, ::string::gpu::buffer froxels,
                ::string::gpu::buffer lights);

    ::string::gpu::device& device_;
    ::string::gpu::shader_program* program_ = nullptr;
    FroxelParams params_{};
};

}  // namespace string::render
