#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string::render
{

// Push constant for the froxel light-binning compute (matches Push in shaders/froxel_cull.slang).
// Moved out of geometry_pass.hpp with the FroxelComponent extraction (brief 11).
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

// Per-frame inputs the froxel binning reads from shared scene state (camera + the local-light buffer,
// both owned by GeometryPass).
struct FroxelParams
{
    glm::mat4 view;
    glm::mat4 inv_proj;
    glm::uvec2 screen;
    float near_plane;
    float far_plane;
    uint32_t light_count;
    VkDeviceAddress lights;   // 0 when there are no local lights
};

// Forward+ froxel light-binning: a dependency-free async-compute chain that bins the scene's local
// lights into per-froxel index lists the lit fragment reads. Owns its compute pipeline + the
// per-frame index buffers; the grid dimensions + froxel buffer address feed SceneData via accessors.
// GeometryPass Phase-1 component (brief 11).
class FroxelComponent
{
    // Brief 16 M2: the froxel index ring is a registry-owned PerFrame buffer now (was a hand-managed
    // [frame] vector over the allocator). Allocated at the deterministic resize point + resolved
    // through the registry, so its address is valid from frame 0 regardless of pass update order —
    // the crash-class fix.
    ::string::gpu::ResourceRegistry* resources_ = nullptr;
    ::string::gpu::buffer froxel_buffer_;
    ::string::gpu::shader_program* program_ = nullptr;
    uint32_t tiles_x_ = 0;
    uint32_t tiles_y_ = 0;
    uint32_t count_ = 0;
    uint32_t capacity_ = 0;   // allocated froxel count (grows with the screen)
    uint32_t frames_in_flight_ = 1;

public:
    // Registers froxel_cull.slang and reserves the per-frame buffer slots (allocation deferred to
    // ensure_capacity() once the screen size is known).
    void init(String::engine_context& context, uint32_t frames_in_flight);
    // (Re)allocate the per-frame index buffers for the current screen if it grew.
    void ensure_capacity(VkExtent2D screen);
    // Bind + dispatch the binning compute for `frame`.
    void record(::string::gpu::command_recorder& recorder, uint16_t frame, const FroxelParams& params);
    // Free the index buffers (the pipeline is torn down by the GeometryPass destructor).
    void destroy();

    bool has_work() const { return program_ != nullptr && count_ > 0 && froxel_buffer_.valid(); }
    bool active(uint16_t frame) const
    {
        return program_ != nullptr && count_ > 0 && froxel_buffer_.valid();
    }
    ::string::gpu::resource_id buffer(uint16_t frame) const
    {
        return froxel_buffer_.valid() ? resources_->physical(froxel_buffer_, frame) : 0;
    }
    // Brief 16: the LOGICAL froxel index-buffer handle (the async pass declares this; the executor
    // resolves the per-frame physical). Invalid until ensure_capacity() has allocated the ring.
    ::string::gpu::buffer handle() const { return froxel_buffer_; }
    VkDeviceAddress froxels_address(uint16_t frame) const;   // 0 when the slot has no buffer
    uint32_t tiles_x() const { return tiles_x_; }
    uint32_t tiles_y() const { return tiles_y_; }
    uint32_t count() const { return count_; }
    ::string::gpu::shader_program* program() const { return program_; }
};

}  // namespace string::render
