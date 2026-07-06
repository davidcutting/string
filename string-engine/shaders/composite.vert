#version 450

// Fullscreen triangle generated from gl_VertexIndex — no vertex buffers.
// Covers the screen with a single oversized triangle; uv spans [0,1] across the quad.
layout(location = 0) out vec2 uv;

void main()
{
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
