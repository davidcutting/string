#version 450

layout(push_constants) uniform GridParams {
    vec2 gridScale;      // Grid spacing
    vec2 gridOffset;     // Pan offset
    vec4 gridColor;      // Grid line color
    vec4 backgroundColor;
    float lineWidth;
    float fadeDistance;
} params;

layout(set = 0, binding = 0) uniform Camera3D {
    vec2 position;
    float zoom;
    float rotation;
} camera;

// A coordinate in image pixel space
layout(location = 0) in vec2 pixel_loc;
// A normalized coordinate from the screen center in clip space
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
