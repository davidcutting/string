#version 450

layout(push_constant) uniform Grid2DParams {
    vec4 background_color;
    vec4 grid_color;
    vec4 border_color;
    vec4 axis_color;
    vec2 grid_resolution;
    vec2 grid_center;
    vec2 grid_size;
    vec2 screen_size;
    float line_width;
    float fade_distance;
    float border_width;
    float axis_width;
    float show_border;
    float show_axes;
} params;

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
    uv *= params.screen_size.x / params.screen_size.y;

    // Convert from clip space [-1,1] to screen space [0, screen_size]
    pixel_loc = (positions[gl_VertexIndex] * 0.5 + 0.5) * params.screen_size;
}
