#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/platform/input_map.hpp>

namespace String
{

// A reusable fly camera (yaw/pitch, Y-up). Movement is driven through named InputMap actions so
// controls are remappable; mouse-look reads InputMap::mouse_delta(). Lives in the engine (not a
// pass) so any pass/scene can share one. Call frame_bounds() to position it around a model's
// AABB, then update() each frame; view_proj() feeds the render.
//
// Expected actions (bind via bind_default_controls, or your own): "move_forward", "move_right",
// "move_up" (axes) and "sprint" (button).
class Camera
{
public:
    // Binds WASD + Space/Ctrl axes and Shift-sprint onto a map — the conventional fly controls.
    static void bind_default_controls(InputMap& map);

    // Positions the camera to frame an AABB: outside it, looking at its centre, with move speed
    // and near/far scaled to the model size. Derives the initial yaw/pitch from the look dir.
    void frame_bounds(const glm::vec3& aabb_min, const glm::vec3& aabb_max);

    // Advances the camera: mouse-look from the map's mouse delta, movement from its axis/sprint
    // actions, then rebuilds view_proj for the given framebuffer aspect ratio.
    void update(const InputMap& input, float delta_time, float aspect);

    glm::mat4 view_proj() const { return view_proj_; }
    glm::vec3 position() const { return position_; }

    void set_fov_degrees(float fov) { fov_degrees_ = fov; }
    void set_move_speed(float speed) { move_speed_ = speed; }

private:
    glm::vec3 position_{ 0.0f };
    float yaw_ = 0.0f;     // radians about +Y
    float pitch_ = 0.0f;   // radians, clamped to +/-89 deg
    float move_speed_ = 1.0f;
    float near_ = 0.1f;
    float far_ = 100.0f;
    float fov_degrees_ = 60.0f;
    float look_sensitivity_ = 0.0025f;
    float sprint_multiplier_ = 4.0f;
    glm::mat4 view_proj_{ 1.0f };
};

}  // namespace String
