#version 450

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    uint primitiveCount;
} pc;

layout(location = 0) out vec2 screenPos;

void main() {
    // Generate fullscreen triangle
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    
    // Convert from clip space [-1,1] to screen space [0, screenSize]
    screenPos = (positions[gl_VertexIndex] * 0.5 + 0.5) * pc.screenSize;
}