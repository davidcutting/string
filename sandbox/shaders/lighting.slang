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
// Aliased view of the SAME bindless table for cube maps (brief 07: the prefiltered sky
// environment). Descriptor aliasing is legal — only slots that actually hold cube views are
// ever indexed through this array.
[[vk::binding(1, 0)]] SamplerCube lit_cube_textures[];

// L2 SH irradiance coefficients (9 RGB), GPU-projected from the sky capture (ibl.slang),
// cosine-convolved and pre-divided by pi: reconstruction returns E/pi directly. Unsized array
// (9 entries live) — a FIXED array inside a pointer-loaded struct ICEs slang 2026.12 when the
// enclosing struct is copied by value (slang-ir.cpp:5677 structType assert).
struct ShBuffer { float4 c[]; };

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
    uint debug_flags;          // bit 0: froxel heatmap; bit 1: white-furnace test (brief 07)
    uint _tail0;
    uint _tail1;

    // Brief 07 dynamic sky IBL (appended — earlier offsets unchanged; lighting_data.hpp asserts).
    ShBuffer *sh;              // L2 SH irradiance coefficients (E/pi reconstruction)
    uint env_slot;             // bindless SamplerCube slot of the prefiltered environment ladder
    uint env_mips;             // ladder mip count (perceptual roughness r -> lod r*(env_mips-1))
    uint dfg_slot;             // bindless Sampler2D slot of the split-sum DFG LUT
    uint _tail2;
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
    float cutout_alpha; // brief 04: if >= 0, clip() base-color alpha below it; < 0 disables the test
};

// --- BRDF terms (brief 07: Filament-reference direct BRDF) -------------------------------------
// Roughness convention (audited): `roughness` parameters are glTF PERCEPTUAL roughness r;
// alpha = r*r is applied inside D/V only. The DFG LUT and the prefilter ladder index by the same
// perceptual r (ibl.slang), so every term agrees on the remap.
float distribution_ggx(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * dd * dd, 1e-7);
}
// Height-correlated Smith visibility (Heitz 2014): V = G2 / (4 NdotV NdotL). Replaces the
// brief-02 separable Schlick-GGX form (whose k = (r+1)^2/8 remap lost grazing energy).
float v_smith_ggx_correlated(float NdotV, float NdotL, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-7);
}
float3 fresnel_schlick(float cos_theta, float3 F0, float f90)
{
    return F0 + (f90 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}
// Energy-renormalized Burley diffuse (Lagarde/Frostbite variant — raw Disney exceeds unity at
// grazing retroreflection). Returns the scalar diffuse factor INCLUDING the 1/pi.
float fd_burley(float NdotV, float NdotL, float LdotH, float roughness)
{
    float energy_bias = lerp(0.0, 0.5, roughness);
    float energy_factor = lerp(1.0, 1.0 / 1.51, roughness);
    float fd90 = energy_bias + 2.0 * LdotH * LdotH * roughness;
    float light_scatter = 1.0 + (fd90 - 1.0) * pow(clamp(1.0 - NdotL, 0.0, 1.0), 5.0);
    float view_scatter = 1.0 + (fd90 - 1.0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    return light_scatter * view_scatter * energy_factor / PI;
}
// One direct light. `energy_comp` is the Kulla-Conty multi-scatter compensation derived from the
// split-sum DFG LUT (1 + F0*(1/(dfg.x+dfg.y) - 1)); `f90` is the roughness-aware grazing
// reflectance (max(1-r, F0): rough dielectrics lose the white grazing sheen).
float3 shade_brdf(float3 N, float3 V, float3 L, float3 diffuse_color, float roughness,
                  float3 F0, float f90, float3 energy_comp, float3 radiance)
{
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float LdotH = max(dot(L, H), 0.0);
    if (NdotL <= 0.0) return float3(0.0);
    float D = distribution_ggx(NdotH, roughness);
    float Vis = v_smith_ggx_correlated(NdotV, NdotL, roughness);
    float3 F = fresnel_schlick(LdotH, F0, f90);
    float3 spec = (D * Vis) * F * energy_comp;
    float3 diff = diffuse_color * fd_burley(NdotV, NdotL, LdotH, roughness);
    return (diff + spec) * radiance * NdotL;
}

// E/pi for normal n from the 9 pre-convolved SH coefficients.
float3 sh_irradiance(ShBuffer *sh, float3 n)
{
    float3 e = sh.c[0].rgb * 0.282095
             + sh.c[1].rgb * (0.488603 * n.y)
             + sh.c[2].rgb * (0.488603 * n.z)
             + sh.c[3].rgb * (0.488603 * n.x)
             + sh.c[4].rgb * (1.092548 * n.x * n.y)
             + sh.c[5].rgb * (1.092548 * n.y * n.z)
             + sh.c[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0))
             + sh.c[7].rgb * (1.092548 * n.x * n.z)
             + sh.c[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(e, float3(0.0));
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
// `blend_alpha` controls the returned alpha: false (default) => opaque (alpha 1, for the opaque/
// cutout/two-sided main pass, whose pipeline blends SRC_ALPHA over dst — a non-1 alpha there would
// wrongly make solid geometry translucent); true => the material's base-color alpha, for the M2
// sorted transparency pass. The base-color alpha is still read for the cutout clip() regardless.
float4 shade_surface(SceneData s, Surface surf, float4 debug_tint = float4(0), bool blend_alpha = false)
{
    float4 base = lit_textures[surf.slots.x].Sample(surf.uv) * surf.base_color;
    // Brief 04 alpha-tested cutout: discard fragments whose base-color alpha is below the material's
    // threshold (foliage/curtains). Threshold < 0 disables (opaque/blend materials).
    if (surf.cutout_alpha >= 0.0 && base.a < surf.cutout_alpha) discard;
    float3 albedo = base.rgb;
    float3 mr = lit_textures[surf.slots.z].Sample(surf.uv).rgb;
    float roughness = clamp(mr.g * surf.metal_rough.y, 0.04, 1.0);
    float metallic = clamp(mr.b * surf.metal_rough.x, 0.0, 1.0);
    float ao = lit_textures[surf.slots.w].Sample(surf.uv).r;

    // Brief 07 white-furnace test (debug_flags bit 1): uniform white environment (the IBL chain
    // captured radiance 1), albedo forced white, AO off, sun + local lights off. A correct,
    // energy-conserving BRDF makes the scene DISAPPEAR into the background at every
    // roughness/metallic — the acceptance gate for the BRDF work itself.
    const bool furnace = (s.debug_flags & 2u) != 0u;
    if (furnace)
    {
        albedo = float3(1.0);
        ao = 1.0;
    }

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

    // Base reflectance. F0 = lerp(0.04, albedo, metallic); the 0.04 dielectric F0 corresponds to
    // Filament's `reflectance` = 0.5 default (F0 = 0.16 * reflectance^2). The Surface carries no
    // per-material reflectance yet — the remap is centralized here for when materials plumb it.
    const float kDielectricF0 = 0.16 * 0.5 * 0.5;   // = 0.04
    float3 F0 = lerp(float3(kDielectricF0), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);
    // Roughness-aware F90 (Lagarde): smooth surfaces keep the full white grazing reflection,
    // rough dielectrics fall back toward F0 (no white sheen at grazing).
    float f90 = clamp(max(max(1.0 - roughness, F0.r), max(F0.g, F0.b)), 0.0, 1.0);
    float NdotV = max(dot(N, V), 1e-4);

    // Split-sum DFG LUT (indexed by NdotV x PERCEPTUAL roughness). dfg.x + dfg.y is the
    // single-scatter directional albedo for F0 = 1 — the Kulla-Conty compensation term.
    float2 dfg = lit_textures[s.dfg_slot].SampleLevel(float2(NdotV, roughness), 0).rg;
    float3 energy_comp = 1.0 + F0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0);

    float view_z = mul(s.view, float4(surf.world_pos, 1.0)).z;

    float3 direct = float3(0.0);
    uint dbg_count = 0;
    if (!furnace)
    {
        float3 L = normalize(s.sun_dir);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0 && s.sun_intensity > 0.0)
        {
            // Directional light: sun_color * sun_intensity is the illuminance (lux-scale) on a
            // surface facing the sun; the BRDF integrates it as E_perp * BRDF * NdotL.
            float3 sun_illuminance = s.sun_color * s.sun_intensity;
            float shadow = shadow_factor(s, surf.world_pos, geom_n, NdotL, view_z);
            direct = shade_brdf(N, V, L, diffuse_color, roughness, F0, f90, energy_comp,
                                sun_illuminance) * (1.0 - shadow);
        }

        uint froxel = froxel_index_for(s, surf.frag_coord, view_z);
        uint stride = 1u + s.max_lights_per_froxel;
        uint base_idx = froxel * stride;
        uint count = s.froxels.data[base_idx];
        dbg_count = count;
        for (uint li = 0; li < count; ++li)
        {
            uint light_id = s.froxels.data[base_idx + 1u + li];
            GpuLight lt = s.lights.lights[light_id];
            float3 to_light = lt.position_radius.xyz - surf.world_pos;
            float dist = length(to_light);
            float range = lt.position_radius.w;
            if (dist > range) continue;
            float3 Ll = to_light / max(dist, 1e-4);
            // Inverse-square with smooth range windowing (Karis window). Brief 07 M4 (falloff
            // audit): the old +1 in the denominator clamped near-field intensity ~arbitrarily;
            // physical punctual falloff is I/d^2 with a small clamp to avoid the singularity.
            float d2 = max(dist * dist, 0.01 * 0.01);
            float win = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
            float atten = win * win / d2;
            if (lt.direction_type.w > 0.5)
            {
                float cd = dot(-Ll, normalize(lt.direction_type.xyz));
                float spot = clamp((cd - lt.cone.y) / max(lt.cone.x - lt.cone.y, 1e-4), 0.0, 1.0);
                atten *= spot * spot;
            }
            // color_intensity.w is luminous-intensity-scale (candela-like): illuminance = I/d^2.
            float3 illuminance = lt.color_intensity.rgb * lt.color_intensity.w * atten;
            direct += shade_brdf(N, V, Ll, diffuse_color, roughness, F0, f90, energy_comp, illuminance);
        }
    }

    // --- Ambient: dynamic sky IBL (brief 07) ----------------------------------------------------
    // Diffuse: L2 SH irradiance (E/pi). Specular: prefiltered GGX ladder, perceptual-roughness
    // mip mapping. Multi-scatter energy compensation per Fdez-Aguera (same DFG LUT), which closes
    // the furnace-test gap for rough metals. AO: baked AO multiplies the diffuse ambient only;
    // specular ambient gets horizon-based specular occlusion derived from AO (Lagarde); direct
    // light is never AO'd.
    float3 R = reflect(-V, N);
    float lod = roughness * float(max(s.env_mips, 1u) - 1u);
    float3 prefiltered = lit_cube_textures[s.env_slot].SampleLevel(R, lod).rgb;
    float3 irradiance = sh_irradiance(s.sh, N);   // E/pi

    float3 FssEss = F0 * dfg.x + f90 * dfg.y;
    float Ess = dfg.x + dfg.y;
    float Ems = 1.0 - Ess;
    float3 Favg = F0 + (f90 - F0) * (1.0 / 21.0);
    float3 Fms = FssEss * Favg / max(1.0 - Ems * Favg, 1e-3);
    float3 kD = diffuse_color * max(1.0 - FssEss - Fms * Ems, 0.0);

    float spec_ao = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    float3 ambient = FssEss * prefiltered * spec_ao + (Fms * Ems + kD) * irradiance * ao;

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

    return float4(color, blend_alpha ? base.a : 1.0);
}
