// Instanced-quad UI (rounded-rect SDF), ported from ui_shader.{vert,frag}.
//
// One instanced quad per shape. Shapes live in a bindless storage buffer (set 0, binding 0)
// selected by the push-constant slot; each instance emits a rect in pixel space converted to
// Vulkan NDC (y-down, so pixel (0,0) = top-left). The fragment stage rounds corners with a
// signed-distance rounded box, draws the border as an inner band, and antialiases the outer edge.

struct Shape
{
    float4 rect;    // xy = pos (px, top-left), zw = size (px)
    float4 fill;    // rgba, linear
    float4 stroke;  // rgba, linear
    float4 params;  // x = corner radius (px), y = stroke width (px)
};

// Bindless storage-buffer array at set 0, binding 0 (matches descriptor_table).
[[vk::binding(0, 0)]]
StructuredBuffer<Shape> shape_buffers[];

struct Push
{
    float2 screen_size;
    uint shape_slot;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float4 fill : COLOR0;
    float4 stroke : COLOR1;
    float2 uv : TEXCOORD0;           // 0..1 within the quad
    float2 size : TEXCOORD1;         // px
    float radius : TEXCOORD2;        // px
    float stroke_width : TEXCOORD3;  // px
};

static const float2 corners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(1, 0), float2(1, 1), float2(0, 1),
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    Shape s = shape_buffers[pc.shape_slot][instance_id];

    float2 corner = corners[vertex_id];
    float2 pixel = s.rect.xy + corner * s.rect.zw;
    float2 ndc = (pixel / pc.screen_size) * 2.0 - 1.0;

    VSOut o;
    o.position = float4(ndc, 0.0, 1.0);
    o.fill = s.fill;
    o.stroke = s.stroke;
    o.uv = corner;
    o.size = s.rect.zw;
    o.radius = s.params.x;
    o.stroke_width = s.params.y;
    return o;
}

// iq's rounded-box SDF: distance from p (relative to the box centre) to a box of half-size b with
// corner radius r. Negative inside, zero on the edge, positive outside.
// https://iquilezles.org/articles/distfunctions2d/
float sd_rounded_box(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - r;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float2 half_size = input.size * 0.5;
    float2 p = (input.uv - 0.5) * input.size;                            // px offset from centre
    float r = min(input.radius, min(half_size.x, half_size.y));         // clamp to fit

    float d = sd_rounded_box(p, half_size, r);

    // Fill by default; where the border band applies (outermost stroke_width px), blend to stroke.
    float4 col = input.fill;
    if (input.stroke_width > 0.0)
    {
        float border = smoothstep(-input.stroke_width - 0.5, -input.stroke_width + 0.5, d);
        col = lerp(input.fill, input.stroke, border);
    }

    // Outer edge: ~1px antialiased coverage of the whole shape.
    float coverage = 1.0 - smoothstep(-0.5, 0.5, d);
    float alpha = col.a * coverage;
    if (alpha <= 0.0)
    {
        discard;
    }
    return float4(col.rgb, alpha);
}
