#version 450

// Instanced-quad UI. The vertex stage resolved each shape's fill/stroke/size/radius; here we
// round the corners with a signed-distance rounded box, draw the border as an inner band, and
// antialias the outer edge. Colors are already linear; the pipeline does the alpha blend.
layout(location = 0) in vec4  frag_fill;
layout(location = 1) in vec4  frag_stroke;
layout(location = 2) in vec2  frag_uv;      // 0..1 within the quad
layout(location = 3) in vec2  frag_size;    // px
layout(location = 4) in float frag_radius;  // px
layout(location = 5) in float frag_stroke_width;  // px

layout(location = 0) out vec4 out_color;

// iq's rounded-box SDF: distance from p (relative to the box centre) to a box of half-size b
// with corner radius r. Negative inside, zero on the edge, positive outside.
// https://iquilezles.org/articles/distfunctions2d/
float sd_rounded_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    vec2 half_size = frag_size * 0.5;
    vec2 p = (frag_uv - 0.5) * frag_size;                       // px offset from centre
    float r = min(frag_radius, min(half_size.x, half_size.y));  // clamp to fit

    float d = sd_rounded_box(p, half_size, r);

    // Fill by default; where the border band applies (outermost stroke_width px, i.e.
    // d > -stroke_width), blend toward the stroke colour.
    vec4 col = frag_fill;
    if (frag_stroke_width > 0.0) {
        float border = smoothstep(-frag_stroke_width - 0.5, -frag_stroke_width + 0.5, d);
        col = mix(frag_fill, frag_stroke, border);
    }

    // Outer edge: ~1px antialiased coverage of the whole shape.
    float coverage = 1.0 - smoothstep(-0.5, 0.5, d);
    float alpha = col.a * coverage;
    if (alpha <= 0.0) {
        discard;
    }

    out_color = vec4(col.rgb, alpha);
}
