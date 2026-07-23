#version 450

// Immediate-mode debug line vertex (brief 06). World position + packed RGBA, transformed by the
// camera view_proj push constant. Drawn as VK_PRIMITIVE_TOPOLOGY_LINE_LIST.
layout(push_constant) uniform DebugLinePush {
    mat4 view_proj;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uint in_rgba;

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = pc.view_proj * vec4(in_pos, 1.0);
    // Unpack RGBA8 to linear-ish float. The offscreen target is linear HDR (composite re-encodes to
    // sRGB), so decode the sRGB debug colour to linear here or it brightens.
    vec4 c = vec4(
        float(in_rgba & 0xffu),
        float((in_rgba >> 8) & 0xffu),
        float((in_rgba >> 16) & 0xffu),
        float((in_rgba >> 24) & 0xffu)) / 255.0;
    vec3 lin = mix(c.rgb / 12.92,
                   pow((c.rgb + 0.055) / 1.055, vec3(2.4)),
                   step(0.04045, c.rgb));
    v_color = vec4(lin, c.a);
}
