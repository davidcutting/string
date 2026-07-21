#version 450
#extension GL_GOOGLE_include_directive : require

#include "sky.glsl"

// Procedural sky background: reconstruct the per-pixel world-space view ray from NDC via the
// inverse view-projection, evaluate the shared sky model, output linear HDR (composite tonemaps).
layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 inv_view_proj;
    vec3 camera_pos;   float _p0;
    vec3 sun_dir;      float _p1;
    vec3 sky_zenith;   float _p2;
    vec3 sky_ground;   float _p3;
    vec3 sun_color;    float _p4;
} pc;

void main()
{
    vec4 world = pc.inv_view_proj * vec4(ndc, 1.0, 1.0);   // unproject a point on the pixel's ray
    vec3 dir = normalize(world.xyz / world.w - pc.camera_pos);
    out_color = vec4(sky(dir, pc.sun_dir, pc.sky_zenith, pc.sky_ground, pc.sun_color), 1.0);
}
