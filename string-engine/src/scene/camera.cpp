#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <string/scene/camera.hpp>

namespace String
{

void Camera::bind_default_controls(InputMap& map)
{
    map.bind_axis("move_forward", KeyCode::W, KeyCode::S);
    map.bind_axis("move_right", KeyCode::D, KeyCode::A);
    map.bind_axis("move_up", KeyCode::SPACE, KeyCode::LEFT_CONTROL);
    map.bind_button("sprint", KeyCode::LEFT_SHIFT);
}

void Camera::frame_bounds(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
{
    const glm::vec3 center = (aabb_min + aabb_max) * 0.5f;
    const float radius = glm::max(glm::length(aabb_max - aabb_min) * 0.5f, 0.001f);

    // Start outside the AABB looking at its centre; derive yaw/pitch from that direction.
    position_ = center + glm::normalize(glm::vec3(1.0f, 0.35f, 1.0f)) * radius * 1.1f;
    const glm::vec3 to_center = glm::normalize(center - position_);
    yaw_ = std::atan2(to_center.z, to_center.x);
    pitch_ = std::asin(glm::clamp(to_center.y, -1.0f, 1.0f));

    move_speed_ = radius * 0.5f;   // ~2 s to cross the scene; sprint is faster
    // near/far are fixed at framing time, but this is a fly camera that roams well past the
    // model's initial bounds — a tight far plane clips distant geometry (a clean cutoff line
    // where a receding surface crosses it). Reverse-Z keeps depth precision excellent even with
    // a very distant far plane, so push it far out to avoid clipping anything the camera reaches.
    near_ = radius * 0.01f;
    far_ = radius * 100.0f;
}

void Camera::update(const InputMap& input, float delta_time, float aspect)
{
    // Mouse-look: yaw from horizontal motion, pitch from vertical (inverted), pitch clamped so we
    // can't flip over. mouse_delta() is zero while the cursor is released (see Input).
    const glm::vec2 mouse = input.mouse_delta();
    yaw_ += mouse.x * look_sensitivity_;
    pitch_ -= mouse.y * look_sensitivity_;
    const float pitch_limit = glm::radians(89.0f);
    pitch_ = glm::clamp(pitch_, -pitch_limit, pitch_limit);

    const glm::vec3 forward = {
        std::cos(pitch_) * std::cos(yaw_),
        std::sin(pitch_),
        std::cos(pitch_) * std::sin(yaw_),
    };
    const glm::vec3 world_up = { 0.0f, 1.0f, 0.0f };
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

    // Movement from remappable axis actions; forward/right in the look plane, up in world space.
    // Only in game mode (cursor captured) — while the cursor is free for UI, WASD belongs to the UI
    // (a focused text field), and the camera holds still. Look already zeroes when free.
    if (input.input().mouse_captured())
    {
        glm::vec3 move = forward * input.axis("move_forward")
                       + right * input.axis("move_right")
                       + world_up * input.axis("move_up");

        float speed = move_speed_;
        if (input.held("sprint"))
        {
            speed *= sprint_multiplier_;
        }
        if (glm::length(move) > 0.0f)
        {
            position_ += glm::normalize(move) * speed * delta_time;
        }
    }

    const float safe_aspect = aspect <= 0.0f ? 1.0f : aspect;
    aspect_ = safe_aspect;
    forward_ = forward;
    const glm::mat4 view = glm::lookAt(position_, position_ + forward, world_up);
    view_ = view;
    glm::mat4 proj = glm::perspective(glm::radians(fov_degrees_), safe_aspect, near_, far_);
    proj[1][1] *= -1;   // GLM is OpenGL-handed; flip Y for Vulkan.

    // Reverse-Z: remap the (ZERO_TO_ONE) NDC depth near->1, far->0 for far better float-depth
    // precision (see the depth pipeline's GREATER_OR_EQUAL compare + 0 clear). Post-multiplying
    // by this sends clip.z -> w - z, i.e. NDC z -> 1 - z.
    glm::mat4 reverse_z(1.0f);
    reverse_z[2][2] = -1.0f;
    reverse_z[3][2] = 1.0f;
    proj = reverse_z * proj;

    view_proj_ = proj * view;
}

}  // namespace String
