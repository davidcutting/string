#version 450
#extension GL_EXT_nonuniform_qualifier : require

// One instanced quad per UI shape. Shapes live in a bindless storage buffer (set 0,
// binding 0) selected by the push-constant slot; each instance reads its own shape and
// emits a rect in pixel space, converted to Vulkan NDC (y-down, so pixel (0,0) = top-left).
struct Shape {
    vec4 rect;    // xy = pos (px, top-left), zw = size (px)
    vec4 fill;    // rgba, linear
    vec4 stroke;  // rgba, linear
    vec4 params;  // x = corner radius (px), y = stroke width (px)
};

layout(set = 0, binding = 0) readonly buffer ShapeBuffer {
    Shape shapes[];
} shape_buffers[];

layout(push_constant) uniform Push {
    vec2 screen_size;
    uint shape_slot;
} pc;

layout(location = 0) out vec4  frag_fill;
layout(location = 1) out vec4  frag_stroke;
layout(location = 2) out vec2  frag_uv;      // 0..1 within the quad
layout(location = 3) out vec2  frag_size;    // px
layout(location = 4) out float frag_radius;  // px
layout(location = 5) out float frag_stroke_width;  // px

void main() {
    const vec2 corners[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(1, 0), vec2(1, 1), vec2(0, 1)
    );

    Shape s = shape_buffers[nonuniformEXT(pc.shape_slot)].shapes[gl_InstanceIndex];

    vec2 corner = corners[gl_VertexIndex];
    vec2 pixel = s.rect.xy + corner * s.rect.zw;
    vec2 ndc = (pixel / pc.screen_size) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_fill = s.fill;
    frag_stroke = s.stroke;
    frag_uv = corner;
    frag_size = s.rect.zw;
    frag_radius = s.params.x;
    frag_stroke_width = s.params.y;
}
