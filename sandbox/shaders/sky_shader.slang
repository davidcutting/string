// Procedural sky background, ported from sky.{vert,frag}. A fullscreen triangle reconstructs the
// per-pixel world-space view ray from NDC via the inverse view-projection, evaluates the shared sky
// model, and outputs linear HDR (composite tonemaps). Depth test/write are off (the pipeline), so
// geometry drawn after overwrites the sky wherever it exists.
import sky;

struct Push
{
    float4x4 inv_view_proj;
    float3 camera_pos;   float _p0;
    float3 sun_dir;      float _p1;
    float3 sky_zenith;   float _p2;
    float3 sky_ground;   float _p3;
    float3 sun_color;    float _p4;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.position = float4(o.ndc, 1.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float4 world = mul(pc.inv_view_proj, float4(input.ndc, 1.0, 1.0));  // unproject a point on the ray
    float3 dir = normalize(world.xyz / world.w - pc.camera_pos);
    return float4(sky(dir, pc.sun_dir, pc.sky_zenith, pc.sky_ground, pc.sun_color), 1.0);
}
