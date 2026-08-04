// Shared Forward+ lit fragment shading (brief 02's BRDF + CSM + froxel local lights + IBL ambient),
// factored out so the task/mesh meshlet path (meshlet_mesh.slang) has ONE lighting implementation.
//
// This module owns the bindless texture array, SceneData/GpuLight/froxel structs, and shade_surface()
// which takes an interpolated surface and returns linear HDR. It declares NO entry points.
import sky;
import probe_common;   // brief 09b: DDGI probe-volume ambient diffuse (grid addressing + sampling)

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
    float3 camera_pos;   float exposure;   // composite EV100 exposure scale (display-referred debug views)
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

    // Brief 09 GTAO (appended — earlier offsets unchanged; lighting_data.hpp asserts). The AO
    // texture holds the PREVIOUS frame's screen-space visibility + world bent normal; sample by
    // reprojecting the world position through prev_view_proj. gtao_slot == 0xFFFFFFFF disables.
    float4x4 prev_view_proj;
    uint gtao_slot;            // half-res texture: rgb = world bent normal * 0.5 + 0.5, a = vis
    float gtao_strength;
    uint gtao_w;               // half-res AO texture size (normal-aware joint upsample)
    uint gtao_h;

    // Brief 09b probe GI (appended — earlier offsets unchanged; lighting_data.hpp asserts). When
    // probe_gi != 0 the ambient DIFFUSE term samples the relit probe volume (Chebyshev-gated
    // 8-probe trilinear) instead of the sky SH; outside the volume / at ~0 weight it falls back to
    // the sky SH exactly as before. probe_gi is 0 when r.gi is off, under r.furnace, or until the
    // capture + first full relight pass have completed — the r.gi-0 path is bit-identical to
    // pre-brief shading (the pixel-parity checkpoint).
    float3 probe_origin;       // world pos of probe (0,0,0)
    float3 probe_spacing;      // per-axis spacing
    uint3 probe_counts;        // per-axis probe counts
    uint probe_irrad_slot;     // irradiance atlas sampled slot
    uint probe_vis_slot;       // visibility atlas sampled slot
    uint probe_gi;             // 0 = off (sky-SH ambient, the pre-09b path)
    ProbeOffsetBuffer *probe_offsets;   // per-probe relocation
    ProbeActiveBuffer *probe_active;    // per-probe activation (inactive probes skipped)
    float probe_occluded_floor;         // fraction of sky-SH ambient a collapsed cage keeps
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
float sample_cascade(SceneData s, uint cascade, float3 world_pos, float3 geom_n, float NdotL, out float2 out_uv)
{
    float3 p = world_pos + geom_n * (s.cascade_texel[cascade].x * s.shadow_normal_offset_scale);
    float4 lp = mul(s.cascade_view_proj[cascade], float4(p, 1.0));
    float3 ndc = lp.xyz / lp.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    out_uv = uv;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return -1.0;
    float frag_depth = ndc.z;
    float bias = s.shadow_bias * (1.0 + 2.0 * (1.0 - NdotL));
    uint slot = s.cascade_slot[cascade].x;
    float shadow = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            // Clamp taps to the map interior so the kernel never reads the cleared border (border = 0
            // = "no occluder" = a false LIT vote, the source of the cascade-edge seam). Reading the
            // nearest real edge texel instead is safe here because shadow_factor's edge_t hands the
            // final few texels off to the larger cascade anyway.
            float2 t = clamp(uv + float2(x, y) * s.shadow_texel, s.shadow_texel, 1.0 - s.shadow_texel);
            // NonUniformResourceIndex: `slot` comes from the PER-PIXEL cascade selection, so it
            // diverges across a wave at cascade boundaries. A divergent bindless index without this
            // qualifier is UB in Vulkan (the GPU scalarizes to one lane's index for all divergent
            // lanes -> wrong cascade read -> non-deterministic shadow flicker). This was THE sparkle.
            float occluder = lit_textures[NonUniformResourceIndex(slot)].SampleLevel(t, 0).r;
            shadow += (frag_depth < occluder - bias) ? 1.0 : 0.0;
        }
    return shadow / 25.0;
}
// Pick the cascade that actually covers `world_pos`. Depth chooses a starting cascade, but the
// receiver can sit outside that cascade's lateral XY footprint (or, for tall/distant occluders, the
// near cascade simply never rendered the caster). Walk UP to larger cascades until one contains the
// point laterally; returns kNoCascade if even the largest doesn't. A smaller cascade being out of
// footprint must NOT be read as "lit" (that was the bright-seam bug).
static const uint kNoCascade = 0xFFFFFFFFu;
uint select_cascade(SceneData s, float3 world_pos, float view_z)
{
    float depth = -view_z;
    uint start = s.cascade_count - 1;
    for (uint i = 0; i < s.cascade_count; ++i)
        if (depth < s.cascade_split[i].x) { start = i; break; }
    for (uint c = start; c < s.cascade_count; ++c)
    {
        float4 lp = mul(s.cascade_view_proj[c], float4(world_pos, 1.0));
        float3 ndc = lp.xyz / lp.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
            return c;
    }
    return kNoCascade;
}

float shadow_factor(SceneData s, float3 world_pos, float3 geom_n, float NdotL, float view_z)
{
    float depth = -view_z;
    uint cascade = select_cascade(s, world_pos, view_z);
    if (cascade == kNoCascade) return 0.0;   // outside every cascade -> unshadowed fill
    float2 uv;
    float sh = sample_cascade(s, cascade, world_pos, geom_n, NdotL, uv);
    if (sh < 0.0) return 0.0;

    // Blend into the next (larger) cascade by the MAX of two weights:
    //  - depth_t: the usual view-depth cross-fade band approaching this cascade's split.
    //  - edge_t:  proximity to this cascade's shadow-map BORDER. The PCF kernel reads a 5x5
    //    neighbourhood, so within a few texels of the [0,1] edge some taps fall off the map into
    //    the cleared border (occluder = 0 = "no occluder") and wrongly vote LIT -- a bright fringe
    //    exactly on the cascade boundary. Fading to the larger cascade (which has this region well
    //    inside its footprint) before the kernel leaves the map removes the seam.
    float split = s.cascade_split[cascade].x;
    if (cascade + 1 < s.cascade_count)
    {
        float band = split * max(s.cascade_blend, 1e-4);
        float depth_t = clamp((depth - (split - band)) / band, 0.0, 1.0);
        // Distance to the nearest shadow-map edge, in TEXELS. The 5x5 PCF kernel reads +-2 texels,
        // so within ~3 texels of the edge some taps fall off the map (cleared border = "no occluder"
        // = wrongly LIT). Hand FULLY to the larger cascade across that off-map zone (edge_t = 1 for
        // <=3 texels), then ramp back to this cascade over the next ~9 texels. A soft ramp (as
        // before) still leaked 30-70% of the lit off-map taps right at the seam.
        float edge_texels = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) / max(s.shadow_texel, 1e-6);
        // edge_t = 1 fully inside the off-map zone (<=3 texels = kernel radius + slack), smoothly
        // ramping to 0 by 14 texels. smoothstep avoids a hard ramp edge that would itself read as a
        // faint line.
        float edge_t = 1.0 - smoothstep(3.0, 14.0, edge_texels);
        float t = max(depth_t, edge_t);
        if (t > 0.0)
        {
            float2 uv_next;
            float sh_next = sample_cascade(s, cascade + 1, world_pos, geom_n, NdotL, uv_next);
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
    float3 dbg_sun = float3(0.0);   // sun-only direct term (debug view)
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
            dbg_sun = shade_brdf(N, V, L, diffuse_color, roughness, F0, f90, energy_comp,
                                 sun_illuminance) * (1.0 - shadow);
            direct = dbg_sun;
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
    // direct light is never AO'd.
    float3 R = reflect(-V, N);
    float lod = roughness * float(max(s.env_mips, 1u) - 1u);
    float3 prefiltered = lit_cube_textures[s.env_slot].SampleLevel(R, lod).rgb;
    float3 irradiance = sh_irradiance(s.sh, N);   // E/pi (sky SH — the fallback + r.gi-0 path)
    float3 sky_diffuse_ref = irradiance;          // open-sky diffuse, kept as the GI-spec-occ reference

    // --- Brief 09b probe GI: the ambient DIFFUSE switches to the relit probe volume -------------
    // Chebyshev-visibility-gated 8-probe trilinear (probe_common) — the visibility test is what
    // keeps courtyard probes from leaking into the interiors. The volume-hull fade blends back to
    // the sky SH over one cell OUTSIDE the grid; a ~0 total weight (pathological: every cell
    // corner inactive/rejected) also falls back. The specular ambient still samples the sky IBL cube,
    // but is now occluded by a GI-derived spec-occ (below) so blocked faces stop reflecting unseen
    // sky; GTAO keeps multiplying below (it is the sub-probe-scale detail layer).
    if (s.probe_gi != 0u)
    {
        ProbeGrid pg;
        pg.origin = s.probe_origin;
        pg.spacing = s.probe_spacing;
        pg.counts = s.probe_counts;
        pg.irrad_stride = 10;
        pg.vis_stride = 18;
        pg.offsets = s.probe_offsets;
        pg.active = s.probe_active;
        float3 cellf = (surf.world_pos - pg.origin) / pg.spacing;
        float3 dmin = min(cellf, float3(pg.counts) - 1.0 - cellf);
        float hull = saturate(min(dmin.x, min(dmin.y, dmin.z)) + 1.0);   // 1 inside, 0 a cell out
        if (hull > 0.0)
        {
            float pw = 0.0;
            float3 pe = probe_irradiance(pg, lit_textures[s.probe_irrad_slot],
                                         lit_textures[s.probe_vis_slot],
                                         surf.world_pos, N, V, pw);
            // Where the cage COLLAPSES (a point occluded from all 8 corners -> pw ~ 0, i.e. a wall or
            // vault thinner than the probe spacing) the probe field has no data. A hard cutoff there
            // snaps between full probe and a fixed floor, which reads as a sharp bright LEAK or a black
            // crevice. Instead ramp SMOOTHLY on the cage weight `pw`: well-covered points take the
            // probe result; as coverage falls, blend toward a dim, normal-shaped stand-in (a fraction
            // of the sky-SH ambient) so the probe term stays a clean LOW-FREQUENCY base with no
            // discontinuity. GTAO then contours it; a future SSGI pass would supply the real near-field
            // detail. The ramp band is anchored low so genuinely well-covered points are untouched.
            float conf = smoothstep(0.01, 0.1, pw);                    // 0 = collapsed, 1 = covered
            float3 floor_ambient = irradiance * s.probe_occluded_floor; // dim sky ambient (N-shaped)
            float3 probe_e = lerp(floor_ambient, pe, conf);
            irradiance = lerp(irradiance, probe_e, hull);
        }
    }
    // GI-debug isolate view (r.gi.debug 1 => probe_gi==2): show ONLY the indirect diffuse response
    // (probe irradiance x albedo), no direct light, no specular ambient — makes a reference-free
    // scene judgeable and lets the user point at a wrong probe. `irradiance` above is the (possibly
    // probe-fed) E/pi term; multiply by the diffuse albedo to get the indirect diffuse radiance.
    if (s.probe_gi == 2u)
        return float4(irradiance * diffuse_color, blend_alpha ? base.a : 1.0);

    float3 FssEss = F0 * dfg.x + f90 * dfg.y;
    float Ess = dfg.x + dfg.y;
    float Ems = 1.0 - Ess;
    float3 Favg = F0 + (f90 - F0) * (1.0 / 21.0);
    float3 Fms = FssEss * Favg / max(1.0 - Ems * Favg, 1e-3);
    float3 kD = diffuse_color * max(1.0 - FssEss - Fms * Ems, 0.0);

    // --- Brief 09 GTAO: screen-space visibility + bent normal, reprojected from the previous
    // frame's AO pass (see SceneData.gtao_slot). Diffuse ambient gets the multi-bounce-corrected
    // visibility (Jimenez cubic fit against the real albedo — avoids over-darkened corners);
    // specular ambient uses the BENT-NORMAL cone vs the reflection lobe (replacing the brief-07
    // AO-derived approximation for the screen-space term). The BAKED material AO keeps its 07
    // rules: diffuse multiply + Lagarde horizon term on specular.
    float3 gtao_diffuse = float3(1.0);
    float gtao_spec = 1.0;
    if (s.gtao_slot != 0xFFFFFFFFu)
    {
        float4 prev_clip = mul(s.prev_view_proj, float4(surf.world_pos, 1.0));
        if (prev_clip.w > 1e-4)
        {
            float2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
            if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0))
            {
                // NORMAL-AWARE JOINT UPSAMPLE of the half-res AO. A plain bilinear sample mixes
                // the 2x2 half-res footprint regardless of what surface each texel belongs to; at
                // silhouettes that always blends this surface's AO with the BACKGROUND's (whose
                // valid bent normal points elsewhere and whose vis may carry real occlusion, e.g.
                // ground contact shadow behind a sphere edge) — the leaked bent cone then reads as
                // dark pixelated edge artifacts on mirror silhouettes. Weight each texel by
                // bilinear coverage x agreement of its bent normal with THIS fragment's geometric
                // normal: cross-surface texels are rejected instead of blended. Sky texels store a
                // zero bent vector, so the un-normalized dot rejects them for free.
                float2 gdim = float2(max(s.gtao_w, 1u), max(s.gtao_h, 1u));
                float2 st = prev_uv * gdim - 0.5;
                float2 gb = floor(st);
                float2 gf = st - gb;
                float4 g = float4(0.0);
                float g_wsum = 0.0;
                [unroll] for (int gy = 0; gy < 2; ++gy)
                    [unroll] for (int gx = 0; gx < 2; ++gx)
                    {
                        float2 tuv = clamp((gb + float2(gx, gy) + 0.5) / gdim,
                                           float2(0.0), float2(1.0));
                        float4 t = lit_textures[s.gtao_slot].SampleLevel(tuv, 0);
                        float3 tb = t.xyz * 2.0 - 1.0;   // un-normalized: length ~1 on surfaces, 0 on sky
                        float w_bil = (gx == 0 ? 1.0 - gf.x : gf.x)
                                    * (gy == 0 ? 1.0 - gf.y : gf.y);
                        float w_n = pow(saturate(dot(tb, geom_n)), 4.0);
                        float w = w_bil * w_n;
                        g += t * w;
                        g_wsum += w;
                    }
                // If NO texel agrees with this surface (the depth min-resolve erodes silhouettes
                // by up to a pixel, so edge fragments can sit entirely over background AO), there
                // is no valid AO information for this fragment. Fall back to NO occlusion — the
                // same rule disocclusions use — instead of averaging the disagreeing background
                // texels, which leaked their occlusion onto mirror silhouettes as dark pixels.
                if (g_wsum > 1e-3)
                {
                g /= g_wsum;
                float vis = 1.0 - s.gtao_strength * (1.0 - g.a);
                // Multi-bounce approximation (Jimenez et al. 2016, eq. 10).
                float3 al = diffuse_color;
                float3 mb = ((vis * (2.0404 * al - 0.3324) + (-4.7951 * al + 0.6417)) * vis
                             + (2.7552 * al + 0.6903)) * vis;
                gtao_diffuse = max(float3(vis), min(mb, float3(1.0)));
                // debug_flags bit 2: disable bent-normal spec-occ (fall back to brief-07's
                // AO-derived Lagarde term, spec_ao below). A/B lever for the mirror defect.
                if ((s.debug_flags & 4u) == 0u) {
                // Bent-normal specular occlusion (Lagarde/"Moving Frostbite to PBR", eq. from the
                // bent-cone construction). The KEY correctness rule: an UNOCCLUDED point (vis==1)
                // must return visibility 1 for the reflection lobe no matter how the surface curves
                // — curvature (the angle between the bent normal and R) is NOT occlusion. The
                // previous cone-intersection form treated a smooth lobe on a curved sphere top as
                // occluded (d_bc ~ angle(N,R) fell inside the ramp), darkening mirror reflections
                // of the open sky. We compute a visibility cone aperture that OPENS to the full
                // hemisphere as vis->1, so the intersection with the (narrow, for low roughness)
                // reflection lobe is unoccluded whenever the point sees the sky.
                // Edge confidence: the AO texture stores NORMALIZED bent vectors on surfaces and
                // an exact ZERO vector on sky texels; the bilinear sample here shortens the
                // decoded vector wherever its half-res footprint straddles a silhouette (sphere
                // edge against sky/background) or mixes disagreeing surfaces. normalize() of that
                // near-zero blend is garbage (NaN at exactly zero) and manufactured a pixelated
                // black rim around the mirror spheres. Use the decoded length as a confidence —
                // ~1 on valid interior texels, collapsing at edges — and fade the occlusion out
                // with it: an unreliable bent direction must never occlude.
                float3 bn_raw = g.xyz * 2.0 - 1.0;
                float bn_len = length(bn_raw);
                float bn_conf = smoothstep(0.5, 0.9, bn_len);
                float3 bent = bn_raw / max(bn_len, 1e-4);
                // Visibility cone half-angle: vis is the fraction of the hemisphere visible around
                // the bent normal; solid angle 2*pi*(1-cos) => cos(aperture) = 1 - vis, aperture
                // ranges 0 (fully occluded) .. pi/2 (open). At vis==1 the cone is the full open
                // hemisphere and cannot occlude anything ahead of the bent normal.
                float cos_av = clamp(1.0 - vis, -1.0, 1.0);
                float a_v = acos(cos_av);
                // Reflection-lobe half-angle: a narrow cone for mirrors, widening with roughness.
                float a_s = clamp(roughness, 0.0, 1.0) * 1.5707963 * 0.5;
                // Angle between the bent (visible) direction and the reflection vector.
                float d_bc = acos(clamp(dot(bent, R), -1.0, 1.0));
                // ONE-SIDED ramp starting AT the cone edge: d_bc <= a_v is exactly unoccluded.
                // A centered ramp ([a_v - a_s, a_v + a_s]) evaluated 0.5 at d_bc == a_v — and at
                // every curved silhouette the incidence goes grazing, so d_bc -> pi/2 while a_v
                // caps at pi/2 even for a fully OPEN point: every unoccluded silhouette pixel got
                // ~half-occluded, which read as a pixelated dark rim around the mirror spheres.
                // The lobe half that dips below the geometric horizon at grazing is already
                // accounted for by the BRDF/prefilter; counting it here double-darkens.
                //
                // RAMP WIDTH = the estimator's noise floor, not just the lobe width. For a fully
                // open surface at GRAZING (every silhouette: gamma -> pi/2 in ALL slices) the
                // 3-slice estimator's worst IGN phase yields vis ~ 0.9 instead of 1 (the sum of
                // per-slice projected-normal weights spans ~1.73..1.97 of the expected 1.91), and
                // the 5x5 denoise cannot fully average it out there because the depth-valid
                // neighborhood at a silhouette is a thin band. vis 0.9 puts a_v at ~84.6 deg while
                // d_bc sits at ~90 deg — a few-degree deficit that a narrow ramp converts into
                // strong phantom occlusion (the surviving pixelated edge artifact). A ~20 deg
                // minimum ramp keeps that deficit imperceptible while real occluders (crevices:
                // d_bc tens of degrees past a_v) still saturate.
                float occ = smoothstep(a_v, a_v + max(a_s, 0.35), d_bc) * bn_conf;
                // Full-visibility gate widened over the same noise band: vis >= ~0.95 is "open
                // within measurement noise" — no occluder information, spec-occ must vanish. The
                // fade starts at 0.8 so the noisy 0.85..0.95 silhouette range is strongly damped;
                // genuinely occluded points (vis 0.3..0.7) keep the full term.
                occ *= 1.0 - smoothstep(0.8, 0.95, vis);
                // Rough lobes are nearly hemispherical — the narrow-cone-at-R model over-occludes
                // them (all-or-nothing occlusion on materials whose ENTIRE env term matters; a
                // drastic appearance flip on rough metals). Blend the cone result toward the
                // plain cosine-hemisphere visibility as roughness rises (Frostbite convention:
                // specular occlusion -> AO at roughness 1).
                float so = lerp(1.0 - occ, vis, roughness);
                gtao_spec = lerp(1.0, so, s.gtao_strength);
                }
                }
            }
        }
    }

    // GI-derived specular occlusion. The probe volume already occluded the DIFFUSE ambient
    // (`irradiance` is dark where the sky is blocked — e.g. an arcade interior face), but the
    // specular ambient samples the GLOBAL prefiltered sky and would otherwise reflect sky the surface
    // cannot see (the "lit occluded face" artifact). Use how much the probe darkened the diffuse vs
    // the open-sky reference as an ambient-occlusion factor for the specular term too, so blocked
    // faces stop reflecting the sky. This is an approximation (diffuse visibility, not the reflection
    // cone) but a standard AO-driven spec-occ, reusing data we already have. GI off -> ratio 1 (no-op).
    float gi_spec_ao = 1.0;
    if (s.probe_gi != 0u)
    {
        const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
        float open = dot(sky_diffuse_ref, kLuma);
        gi_spec_ao = open > 1e-5 ? saturate(dot(irradiance, kLuma) / open) : 1.0;
    }

    float spec_ao = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    float3 ambient = FssEss * prefiltered * (spec_ao * gtao_spec * gi_spec_ao)
                   + (Fms * Ems + kD) * irradiance * (ao * gtao_diffuse);

    float3 color = direct + ambient;

    // Lighting isolate views (debug_flags bits 4..7 <- r.debug.lighting): decompose what actually
    // lights a pixel so a "wrongly lit" surface can be attributed without guessing. 1/2/3 are raw HDR
    // (composite tonemaps them like the real frame); 4 is a scaled B/W shadow-coverage view.
    //   1 = sun direct only (cast shadows read as BLACK inside sun-lit surfaces)
    //   2 = ambient only (sky IBL diffuse + specular)
    //   3 = local (froxel) lights only
    //   4 = sun shadow coverage: WHITE = sun-lit, BLACK = in shadow / back-facing (scaled so EV100
    //       exposure + ACES can't crush it)
    //   5 = selected shadow cascade: RED=0, GREEN=1, BLUE=2, YELLOW=3, MAGENTA=outside all cascades
    //   6 = occluder probe: GREEN=caster above pixel (should be shadowed), RED=pixel topmost (lit /
    //       occluder not rasterized here), MAGENTA=map empty at this uv, BLUE=uncovered
    //   7/8 = raw shadow-map depth of cascade 0 / 1 by screen uv (bright=near sun, black=empty)
    uint light_dbg = (s.debug_flags >> 4) & 0xFu;
    if (light_dbg == 1u) return float4(dbg_sun, 1.0);
    if (light_dbg == 2u) return float4(ambient, 1.0);
    if (light_dbg == 3u) return float4(max(direct - dbg_sun, float3(0.0)), 1.0);
    if (light_dbg == 4u)
    {
        float3 Ld = normalize(s.sun_dir);
        float ndl = max(dot(N, Ld), 0.0);
        if (ndl <= 0.0 || s.sun_intensity <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
        float sh = shadow_factor(s, surf.world_pos, geom_n, ndl, view_z);
        return float4(float3(1.0 - sh) * 1e6, 1.0);   // white = lit, black = shadowed
    }
    if (light_dbg == 5u)
    {
        // Categorical cascade colours must keep their HUE through the composite. ACES desaturates
        // bright saturated values toward white (why the 1e6 trick can't be used here), so instead
        // emit DISPLAY-REFERRED colours: multiply by inv_exposure to cancel the composite's EV100
        // exposure, so the LUT sees the literal ~1.0 primary and preserves the hue. Same technique
        // the UI/text shaders use for display-referred colour.
        float inv_exposure = 1.0 / max(s.exposure, 1e-8);
        uint c = select_cascade(s, surf.world_pos, view_z);
        float3 col = float3(1.0, 0.0, 1.0);   // magenta = outside all cascades
        if (c != kNoCascade)
        {
            float3 cols[4] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0) };
            col = cols[min(c, 3u)];
        }
        return float4(col * inv_exposure, 1.0);
    }
    if (light_dbg == 6u)
    {
        // Occluder probe (3-way). Reproject into the selected cascade, sample the map's nearest-to-sun
        // depth at this uv (reverse-Z: nearer sun = larger; cleared = 0), compare to the fragment's
        // own depth.
        //   GREEN   = occluder well above this pixel -> it SHOULD be shadowed.
        //   RED     = occluder ~= this pixel's depth -> the pixel IS the topmost surface in the map
        //             -> genuinely lit (the OCCLUDER specifically was not rasterized here).
        //   MAGENTA = map is EMPTY at this uv (occluder ~= 0) -> NOTHING rendered here, not even the
        //             receiver itself -> a coverage / cull / matrix-mismatch failure for the cascade.
        //   BLUE    = pixel outside every cascade.
        float inv_exposure = 1.0 / max(s.exposure, 1e-8);
        uint c = select_cascade(s, surf.world_pos, view_z);
        if (c == kNoCascade) return float4(float3(0, 0, 1) * inv_exposure, 1.0);
        float3 p = surf.world_pos + geom_n * (s.cascade_texel[c].x * s.shadow_normal_offset_scale);
        float4 lp = mul(s.cascade_view_proj[c], float4(p, 1.0));
        float3 ndc = lp.xyz / lp.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        uint slot = s.cascade_slot[c].x;
        float occluder = lit_textures[NonUniformResourceIndex(slot)].SampleLevel(uv, 0).r;
        float delta = occluder - ndc.z;                 // >0 => something is nearer the sun than us
        float3 col;
        if (occluder < 0.001)      col = float3(1, 0, 1);   // magenta: empty map at this uv
        else if (delta > 0.002)    col = float3(0, 1, 0);   // green: occluder above -> shadowed
        else                       col = float3(1, 0, 0);   // red: topmost surface -> lit
        return float4(col * inv_exposure, 1.0);
    }
    if (light_dbg == 7u || light_dbg == 8u)
    {
        // Raw shadow-map viewer: sample a cascade's depth directly by SCREEN uv (NOT reprojected),
        // showing the map in its own light-space. Reverse-Z: bright = near the sun (tall casters),
        // dark = far, BLACK = empty/unrendered. 7 = cascade 0, 8 = cascade 1. Use this to confirm
        // whether the roof silhouette is actually present in the map at all.
        float inv_exposure = 1.0 / max(s.exposure, 1e-8);
        float2 res = float2(s.froxel_dims.x * s.froxel_dims.w, s.froxel_dims.y * s.froxel_dims.w);
        float2 suv = surf.frag_coord.xy / max(res, float2(1.0));
        uint ci = light_dbg == 7u ? 0u : 1u;
        uint slot = s.cascade_slot[min(ci, s.cascade_count - 1)].x;
        float d = lit_textures[slot].SampleLevel(suv, 0).r;
        return float4(float3(d) * inv_exposure, 1.0);
    }

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
