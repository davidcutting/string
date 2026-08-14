#include <string/scene/camera.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>

namespace
{

// frame_bounds must place the camera OUTSIDE the AABB, looking at its centre, with near/far
// scaled to contain the model. These pins are what the sandbox relies on for initial framing.
TEST(Camera, FrameBoundsPositionsOutsideBox)
{
    string::Camera cam;
    const glm::vec3 mn{ -2.0f, 0.0f, -3.0f };
    const glm::vec3 mx{ 4.0f, 5.0f, 3.0f };
    cam.frame_bounds(mn, mx);

    const glm::vec3 p = cam.position();
    const bool inside = p.x > mn.x && p.x < mx.x && p.y > mn.y && p.y < mx.y && p.z > mn.z
                        && p.z < mx.z;
    EXPECT_FALSE(inside);
    EXPECT_LT(cam.near_plane(), cam.far_plane());
    EXPECT_GT(cam.far_plane(), 0.0f);

    // The look direction (derived from the framed yaw/pitch — forward() is only rebuilt in
    // update()) points from the camera towards the box centre.
    const glm::vec3 centre = 0.5f * (mn + mx);
    const glm::vec3 to_centre = glm::normalize(centre - p);
    const glm::vec3 look{ std::cos(cam.pitch()) * std::cos(cam.yaw()), std::sin(cam.pitch()),
                          std::cos(cam.pitch()) * std::sin(cam.yaw()) };
    EXPECT_GT(glm::dot(look, to_centre), 0.9f);
}

TEST(Camera, SetPoseIsExact)
{
    string::Camera cam;
    const glm::vec3 p{ 1.0f, 2.0f, 3.0f };
    cam.set_pose(p, 0.5f, -0.25f);
    EXPECT_EQ(cam.position(), p);
    EXPECT_FLOAT_EQ(cam.yaw(), 0.5f);
    EXPECT_FLOAT_EQ(cam.pitch(), -0.25f);
}

}  // namespace
