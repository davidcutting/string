// Per-frame scene data + the light/froxel structs it points at (brief 23 M3a: moved verbatim
// out of lighting.slang so depth-only consumers — meshlet_shadow — can reach SceneData without
// importing the whole lighting module, which would drag sky/probe imports and a second bindless
// array declaration onto their pipeline layouts). Pure data declarations: NO bindings, NO entry
// points. lighting.slang re-exports this module, so its importers see these types unchanged.
import probe_common;   // ProbeOffsetBuffer / ProbeActiveBuffer (pointer members below)

static const uint kMaxCascades = 4;

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

// --- Brief 23: the skinned second vertex stream + joint palettes -------------------------------
// One SkinVertex per skinned vertex, 8B packed into a uint2: x = 4 joint bytes (indices into the
// draw's palette), y = 4 unorm8 weight bytes SUMMING TO EXACTLY 255 by cook-time construction —
// the shader does NOT renormalize. The heap is compact (skinned vertices only); DrawInfo's
// skin_offset rebases the ORIGINAL global vertex index into it, the vertex_offset idiom.
// Declared HERE (not meshlet.slang) because SceneData carries the two pointers below.
struct SkinVertexBuffer { uint2 v[]; };
struct PaletteBuffer { float4x4 m[]; };

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

    // Brief 23 skinning (appended — earlier offsets unchanged; lighting_data.hpp asserts). The
    // palette pointer is per FRAME SLOT (the app-backed ring rotates; palette_offset in DrawInfo
    // does not) — which is exactly why these live in the ringed SceneData and not the draw table.
    // Null (0) when the scene has no skinned geometry; load_vertex only dereferences behind
    // di.skinned.
    SkinVertexBuffer *skin_stream;
    PaletteBuffer *joint_palette;
};
