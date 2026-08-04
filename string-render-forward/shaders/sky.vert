#version 450

// Fullscreen triangle for the sky background; passes NDC xy so the fragment can reconstruct a
// world-space view ray. Depth test/write are off (the pipeline), so geometry drawn after overwrites
// the sky wherever it exists.
layout(location = 0) out vec2 ndc;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
