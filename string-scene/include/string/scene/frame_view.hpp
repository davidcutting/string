#pragma once

#include <cstdint>
#include <span>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/core/gpu_types.hpp>

#include <string/scene/asset_handles.hpp>

namespace string::scene
{

// The world's environment: sun/time-of-day, sky palette, and the scene-level lighting switches.
// Owned by the world (SCENE state — was 10 loose members on the renderer's GeometryScene); the
// sun fields are DERIVED from time_of_day each tick (see world::tick), the rest are inputs.
struct environment
{
    float time_of_day = 0.30f;
    bool animate_sun = false;
    // The sun arc's southward lean (radians): smaller = higher noon sun. An input the app may
    // drive from its tuning surface (the demo mirrors the r.sun.lean cvar into it).
    float sun_lean = 0.6f;
    // Derived from time_of_day (world::tick): direction TO the sun + the palette.
    glm::vec3 sun_dir{ 0.0f, 1.0f, 0.0f };
    glm::vec3 sun_color{ 1.0f };
    float sun_intensity = 100.0f;
    glm::vec3 sky_zenith{ 2.8f, 6.0f, 12.4f };
    glm::vec3 sky_ground{ 0.0824f, 0.0699f, 0.0503f };
    // Scene-level switches (were renderer key-toggle members).
    bool lights_enabled = true;
    bool furnace = false;   // the white-furnace calibration environment (r.furnace)
};

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

// The per-frame value snapshot of the world that the renderer reads; a pass receives this (or
// the bridge's derived scene_frame) by const-ref in tick() and owns none of it.
struct frame_view
{
    // The view: the world's camera, flattened to values (the client drives Camera itself).
    glm::mat4 view{ 1.0f };
    glm::mat4 view_proj{ 1.0f };
    glm::vec3 camera_pos{ 0.0f };
    float near_plane = 0.1f;
    float far_plane = 100.0f;
    float fov_degrees = 60.0f;
    float aspect = 1.0f;
    // The presentation extent (the app pushes it each frame — the world cannot know the window).
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;

    environment env;

    std::span<const instance_row> instances;   // dense, spawn order
    std::span<const GpuLight> lights;
    // Bumped whenever the ROW SET changes (spawn/despawn/visibility) — consumers rebuild their
    // derived tables on change and skip the rebuild otherwise. Transform-only updates do not bump
    // it; they are re-read every frame.
    uint64_t revision = 0;
};

}  // namespace string::scene
