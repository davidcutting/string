// Shared Forward+ lit fragment shading (brief 02's BRDF + CSM + froxel local lights + IBL ambient),
// factored out so the task/mesh meshlet path (meshlet_mesh.slang) has ONE lighting implementation.
//
// This module owns the bindless texture array, SceneData/GpuLight/froxel structs, and shade_surface()
// which takes an interpolated surface and returns linear HDR. It declares NO entry points.
import sky;

static const float PI = 3.14159265359;
static const uint kMaxCascades = 4;

// --- Bindless textures (set 0, binding 1) ----------------------------------------------------
[[vk::binding(1, 0)]] Sampler2D lit_textures[];

// --- Lights + froxels (reached by device address from SceneData) ------------------------------
struct GpuLight
{
    float4 position_radius;
    float4 color_intensity;
    float4 direction_type;
    float4 cone;
};
struct LightBuffer { GpuLight lights[]; };
struct FroxelBuffer { uint data[]; };

// --- Per-frame scene data (reached by device address; NATURAL layout — see lighting_data.hpp) -----
struct SceneData
{
    float3 camera_pos;   float _p0;
    float3 sun_dir;      float sun_intensity;
    float3 sun_color;    float _p1;
    float3 ambient_sky;  float _p2;
    float3 ambient_ground; float _p3;

    float4x4 cascade_view_proj[kMaxCascades];
    float4 cascade_split[kMaxCascades];
    float4 cascade_texel[kMaxCascades];
    uint4 cascade_slot[kMaxCascades];
    uint cascade_count;
    float shadow_texel;
    float shadow_bias;
    float shadow_normal_offset_scale;
    float cascade_blend;

    float4x4 view;
    uint4 froxel_dims;
    float4 froxel_planes;
    LightBuffer *lights;
    FroxelBuffer *froxels;
    uint max_lights_per_froxel;
    uint debug_flags;
    uint _tail0;
    uint _tail1;
};

// The interpolated surface the fragment shades. Both front-ends fill this.
struct Surface
{
    float2 uv;
    float3 world_pos;
    float3 world_normal;
    float3 world_tangent;
    float tangent_sign;
    float4 base_color;
    uint4 slots;        // base, normal, mr, occlusion
    float2 metal_rough;
    float4 frag_coord;  // SV_Position (for froxel lookup)
};

// --- BRDF terms -------------------------------------------------------------------------------
float distribution_ggx(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * dd * dd, 1e-7);
}
float geometry_smith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}
float3 fresnel_schlick(float cos_theta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}
float3 shade_brdf(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness,
                  float3 F0, float3 radiance)
{
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    if (NdotL <= 0.0) return float3(0.0);
    float D = distribution_ggx(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    float3 F = fresnel_schlick(VdotH, F0);
    float3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (float3(1.0) - F) * (1.0 - metallic);
    float3 diff = kd * albedo / PI;
    return (diff + spec) * radiance * NdotL;
}

// --- Shadows (cascaded) -----------------------------------------------------------------------
float sample_cascade(SceneData s, uint cascade, float3 world_pos, float3 geom_n, float NdotL)
{
    float3 p = world_pos + geom_n * (s.cascade_texel[cascade].x * s.shadow_normal_offset_scale);
    float4 lp = mul(s.cascade_view_proj[cascade], float4(p, 1.0));
    float3 ndc = lp.xyz / lp.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return -1.0;
    float frag_depth = ndc.z;
    float bias = s.shadow_bias * (1.0 + 2.0 * (1.0 - NdotL));
    uint slot = s.cascade_slot[cascade].x;
    float shadow = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            float occluder = lit_textures[slot].Sample(uv + float2(x, y) * s.shadow_texel).r;
            shadow += (frag_depth < occluder - bias) ? 1.0 : 0.0;
        }
    return shadow / 25.0;
}
float shadow_factor(SceneData s, float3 world_pos, float3 geom_n, float NdotL, float view_z)
{
    float depth = -view_z;
    uint cascade = s.cascade_count - 1;
    for (uint i = 0; i < s.cascade_count; ++i)
        if (depth < s.cascade_split[i].x) { cascade = i; break; }
    float sh = sample_cascade(s, cascade, world_pos, geom_n, NdotL);
    if (sh < 0.0) return 0.0;
    float split = s.cascade_split[cascade].x;
    if (cascade + 1 < s.cascade_count && s.cascade_blend > 0.0)
    {
        float band = split * s.cascade_blend;
        float t = clamp((depth - (split - band)) / max(band, 1e-4), 0.0, 1.0);
        if (t > 0.0)
        {
            float sh_next = sample_cascade(s, cascade + 1, world_pos, geom_n, NdotL);
            if (sh_next >= 0.0) sh = lerp(sh, sh_next, t);
        }
    }
    return sh;
}

// --- Froxel local lights ----------------------------------------------------------------------
uint froxel_index_for(SceneData s, float4 frag_coord, float view_z)
{
    uint tx = min(uint(frag_coord.x) / s.froxel_dims.w, s.froxel_dims.x - 1);
    uint ty = min(uint(frag_coord.y) / s.froxel_dims.w, s.froxel_dims.y - 1);
    float depth = max(-view_z, s.froxel_planes.x);
    float slice_f = log(depth / s.froxel_planes.x) * s.froxel_planes.z * float(s.froxel_dims.z);
    uint tz = min(uint(max(slice_f, 0.0)), s.froxel_dims.z - 1);
    return (tz * s.froxel_dims.y + ty) * s.froxel_dims.x + tx;
}

// Shade one surface. `debug_tint` (rgb) is mixed in when its .a > 0 (meshlet/LOD/occlusion views).
float4 shade_surface(SceneData s, Surface surf, float4 debug_tint = float4(0))
{
    float4 base = lit_textures[surf.slots.x].Sample(surf.uv) * surf.base_color;
    float3 albedo = base.rgb;
    float3 mr = lit_textures[surf.slots.z].Sample(surf.uv).rgb;
    float roughness = clamp(mr.g * surf.metal_rough.y, 0.04, 1.0);
    float metallic = clamp(mr.b * surf.metal_rough.x, 0.0, 1.0);
    float ao = lit_textures[surf.slots.w].Sample(surf.uv).r;

    float3 geom_n = normalize(surf.world_normal);
    float3 V = normalize(s.camera_pos - surf.world_pos);
    if (dot(geom_n, V) < 0.0) geom_n = -geom_n;

    float3 T = normalize(surf.world_tangent - geom_n * dot(geom_n, surf.world_tangent));
    float3 B = cross(geom_n, T) * surf.tangent_sign;
    float3 n_ts = lit_textures[surf.slots.y].Sample(surf.uv).xyz * 2.0 - 1.0;
    float3 N = normalize(n_ts.x * T + n_ts.y * B + n_ts.z * geom_n);

    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float variance = 0.25 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float kernel_roughness = min(2.0 * variance, 0.18);
    roughness = clamp(sqrt(roughness * roughness + kernel_roughness), 0.04, 1.0);

    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float view_z = mul(s.view, float4(surf.world_pos, 1.0)).z;

    float3 L = normalize(s.sun_dir);
    float3 sun_radiance = s.sun_color * s.sun_intensity;
    float NdotL = max(dot(N, L), 0.0);
    float3 direct = float3(0.0);
    if (NdotL > 0.0)
    {
        float shadow = shadow_factor(s, surf.world_pos, geom_n, NdotL, view_z);
        direct = shade_brdf(N, V, L, albedo, metallic, roughness, F0, sun_radiance) * (1.0 - shadow);
    }

    uint froxel = froxel_index_for(s, surf.frag_coord, view_z);
    uint stride = 1u + s.max_lights_per_froxel;
    uint base_idx = froxel * stride;
    uint count = s.froxels.data[base_idx];
    uint dbg_count = count;
    for (uint li = 0; li < count; ++li)
    {
        uint light_id = s.froxels.data[base_idx + 1u + li];
        GpuLight lt = s.lights.lights[light_id];
        float3 to_light = lt.position_radius.xyz - surf.world_pos;
        float dist = length(to_light);
        float range = lt.position_radius.w;
        if (dist > range) continue;
        float3 Ll = to_light / max(dist, 1e-4);
        float d2 = dist * dist;
        float win = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float atten = win * win / (d2 + 1.0);
        if (lt.direction_type.w > 0.5)
        {
            float cd = dot(-Ll, normalize(lt.direction_type.xyz));
            float spot = clamp((cd - lt.cone.y) / max(lt.cone.x - lt.cone.y, 1e-4), 0.0, 1.0);
            atten *= spot * spot;
        }
        float3 radiance = lt.color_intensity.rgb * lt.color_intensity.w * atten;
        direct += shade_brdf(N, V, Ll, albedo, metallic, roughness, F0, radiance);
    }

    float3 irradiance = sky_gradient(N, s.ambient_sky, s.ambient_ground);
    float3 diffuse_ibl = irradiance * albedo * (1.0 - metallic);
    float3 R = reflect(-V, N);
    float3 env = lerp(sky(R, s.sun_dir, s.ambient_sky, s.ambient_ground, s.sun_color), irradiance, roughness);
    float NdotV = max(dot(N, V), 1e-4);
    float3 F_amb = F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    float3 specular_ibl = env * F_amb;
    float spec_ao = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    const float kIblStrength = 0.5;
    float3 ambient = (diffuse_ibl * ao + specular_ibl * spec_ao) * kIblStrength;

    float3 color = direct + ambient;

    if ((s.debug_flags & 1u) != 0u)
    {
        float t = clamp(float(dbg_count) / 32.0, 0.0, 1.0);
        float3 heat = lerp(float3(0, 0, 1), lerp(float3(0, 1, 0), float3(1, 0, 0), clamp(t * 2.0 - 1.0, 0.0, 1.0)),
                           clamp(t * 2.0, 0.0, 1.0));
        color = lerp(color, heat, 0.75);
    }
    // Meshlet / LOD / occlusion debug views: flat tint mixed over the shaded colour.
    if (debug_tint.a > 0.0) color = lerp(color, debug_tint.rgb, debug_tint.a);

    return float4(color, base.a);
}
