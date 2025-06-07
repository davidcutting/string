#version 450

layout(push_constants) uniform GridParams {
    vec4 background_color;
    vec4 grid_color;
    vec2 grid_resolution;
    vec2 grid_position;
    float line_width;
    float fade_distance;
} params;

layout(location = 1) in vec2 uv;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 gridPos = (uv - 0.5) * params.gridScale + params.gridOffset;
    
    // Calculate grid lines using fwidth for anti-aliasing
    vec2 grid = abs(fract(gridPos - 0.5) - 0.5) / fwidth(gridPos);
    float line = min(grid.x, grid.y);
    
    // Smooth step for anti-aliased lines
    float gridMask = 1.0 - min(line, 1.0);
    gridMask = smoothstep(0.0, params.lineWidth, gridMask);
    
    // Optional: fade grid at distance
    float fade = 1.0 - smoothstep(0.0, params.fadeDistance, length(gridPos));
    gridMask *= fade;
    
    fragColor = mix(params.backgroundColor, params.gridColor, gridMask);
}
