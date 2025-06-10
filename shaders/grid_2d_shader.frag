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

layout(location = 0) in vec2 pixel_loc;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 grid_pos = (pixel_loc - params.grid_center) / params.grid_resolution;
    vec2 grid_half_size = params.grid_size * 0.5;
    vec2 local_pos = pixel_loc - params.grid_center;
    
    // Check if we're outside the grid area
    if (abs(local_pos.x) > grid_half_size.x || abs(local_pos.y) > grid_half_size.y) {
        fragColor = params.background_color;
        return;
    }
    
    // Check if we're in the border area
    if (params.show_border > 0.5) {
        vec2 border_inner = grid_half_size - vec2(params.border_width);
        if (abs(local_pos.x) > border_inner.x || abs(local_pos.y) > border_inner.y) {
            fragColor = params.border_color;
            return;
        }
    }
    
    // Check for axis lines (simple distance check from grid center)
    if (params.show_axes > 0.5) {
        float axis_thickness = params.axis_width;
        bool on_x_axis = abs(local_pos.y) < axis_thickness;
        bool on_y_axis = abs(local_pos.x) < axis_thickness;
        
        if (on_x_axis || on_y_axis) {
            fragColor = params.axis_color;
            return;
        }
    }
    
    // Calculate grid lines using fwidth for anti-aliasing
    vec2 grid = abs(fract(grid_pos - 0.5) - 0.5) / fwidth(grid_pos);
    float line = min(grid.x, grid.y);
    
    // Smooth step for anti-aliased grid lines
    float grid_mask = 1.0 - min(line, 1.0);
    grid_mask = smoothstep(0.0, params.line_width, grid_mask);
    
    // Optional: fade grid at distance from center
    float fade = 1.0 - smoothstep(0.0, params.fade_distance, length(grid_pos));
    grid_mask *= fade;
    
    fragColor = mix(params.background_color, params.grid_color, grid_mask);
}
