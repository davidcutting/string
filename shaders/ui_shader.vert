#version 450

layout(push_constant) uniform PushConstants {
    vec2 screen_size;
    uint num_shapes;
    float delta_time;
} pc;

// A coordinate in image pixel space
layout(location = 0) out vec2 pixel_loc;
// A normalized coordinate from the screen center in clip space
layout(location = 1) out vec2 uv;

void main() {
    // Generate fullscreen triangle
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    
    uv = positions[gl_VertexIndex];
    // Apply aspect ratio correction to uv
    uv *= pc.screen_size.x / pc.screen_size.y;

    // Convert from clip space [-1,1] to screen space [0, screen_size]
    pixel_loc = (positions[gl_VertexIndex] * 0.5 + 0.5) * pc.screen_size;
}