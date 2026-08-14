#pragma once

#include <cstdint>
#include <span>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/core/gpu_types.hpp>

#include <string/scene/asset_handles.hpp>

namespace string::scene
{

// One spawned instance, as the renderer consumes it: a whole-asset placement (the spec's
// "Instance = mesh-part set + material refs + transform"; parts are internal rows the render
// bridge expands through the asset registry). Value snapshot — a pass never touches world state.
struct instance_row
{
    assets::asset_id asset;
    glm::mat4 transform{ 1.0f };   // world space (hierarchy already flushed)
    uint32_t visible = 1;
    uint32_t entity_index = 0;     // provenance (debug names, inspector); not a lifetime handle
};

// The per-frame value snapshot of the world that the renderer reads. Grows toward the full plan
// shape (camera/environment arrive when they move out of the renderer); a pass receives this (or
// the bridge's derived scene_frame) by const-ref in tick() and owns none of it.
struct frame_view
{
    std::span<const instance_row> instances;   // dense, spawn order
    std::span<const GpuLight> lights;
    // Bumped whenever the ROW SET changes (spawn/despawn/visibility) — consumers rebuild their
    // derived tables on change and skip the rebuild otherwise. Transform-only updates do not bump
    // it; they are re-read every frame.
    uint64_t revision = 0;
};

}  // namespace string::scene
