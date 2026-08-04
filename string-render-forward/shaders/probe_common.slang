// Brief 09b — shared probe-volume math: octahedral (de)encoding, grid addressing, atlas tile
// mapping, and the DDGI 8-probe trilinear + Chebyshev-visibility irradiance sample. Imported by
// probe_relight.slang (produces the atlas), probe_debug.slang (visualizes it) and lighting.slang
// (consumes it). ONE implementation so producer and consumer agree on every convention (Majercik
// et al. 2019 "Dynamic Diffuse Global Illumination" conventions).
//
// Octahedral mapping (Cigolle 2014): a unit direction <-> [0,1]^2 square, equal-area-ish, used for
// both the per-probe irradiance (8x8) and visibility/capture (16x16) tiles. Each tile has a 1-texel
// gutter replicated from the opposite octa edge so hardware bilinear across a tile wraps correctly.

static const float PROBE_PI = 3.14159265359;

// Per-probe relocation offset (RTXGI): the effective probe world position = grid position + offset.
// Written by the capture collapse (probe_capture.slang) to nudge probes out of / away from geometry.
struct ProbeOffsetBuffer { float4 offset[]; };
// Per-probe activation (DDGI probe states): 1 = active, 0 = inside geometry (classification).
// Inactive probes are skipped by relight AND by the sampling weight below (their atlas texels are
// stale/black — contributing them would darken every cell that touches a wall).
struct ProbeActiveBuffer { uint state[]; };

struct ProbeGrid
{
    float3 origin;      // world pos of probe (0,0,0)
    float3 spacing;     // per-axis world spacing
    uint3 counts;       // per-axis probe counts
    uint irrad_stride;  // 10 (8 + gutter)
    uint vis_stride;    // 18 (16 + gutter)
    // Relocation offsets (may be null: probe_world then returns the bare grid position). When set,
    // probe_world adds offset[linear] so every consumer sees the SAME relocated position.
    ProbeOffsetBuffer *offsets;
    // Activation states (may be null: all probes treated active).
    ProbeActiveBuffer *active;
};

// --- Octahedral encode/decode -------------------------------------------------------------------
float2 oct_sign_not_zero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
// dir (unit) -> [-1,1]^2 octahedral coord.
float2 oct_encode(float3 dir)
{
    float3 n = dir / (abs(dir.x) + abs(dir.y) + abs(dir.z));
    float2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * oct_sign_not_zero(n.xy);
    return e;
}
// [-1,1]^2 octahedral coord -> unit dir.
float3 oct_decode(float2 e)
{
    float3 v = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * oct_sign_not_zero(v.xy);
    return normalize(v);
}

// --- Grid addressing ----------------------------------------------------------------------------
uint probe_linear(ProbeGrid g, uint3 c)
{
    return (c.z * g.counts.y + c.y) * g.counts.x + c.x;
}
uint3 probe_coord(ProbeGrid g, uint linear)
{
    uint x = linear % g.counts.x;
    uint y = (linear / g.counts.x) % g.counts.y;
    uint z = linear / (g.counts.x * g.counts.y);
    return uint3(x, y, z);
}
float3 probe_world(ProbeGrid g, uint3 c)
{
    float3 p = g.origin + float3(c) * g.spacing;
    if (g.offsets != nullptr)
    {
        uint linear = probe_linear(g, c);
        p += g.offsets.offset[linear].xyz;
    }
    return p;
}

// Atlas tile origin (top-left inner texel, i.e. AFTER the gutter) for a probe, in texels. Tiles are
// packed: X axis = (counts.x * counts.y) columns, Y axis = counts.z rows. Column index = y*counts.x
// + x; row index = z. `stride` includes the gutter (10 or 18); inner origin is +1 into the tile.
uint2 probe_tile_inner_origin(ProbeGrid g, uint3 c, uint stride)
{
    uint col = c.y * g.counts.x + c.x;
    uint row = c.z;
    return uint2(col * stride, row * stride) + uint2(1, 1);
}
// Full atlas dimensions in texels for a given inner size + stride.
uint2 probe_atlas_size(ProbeGrid g, uint stride)
{
    return uint2(g.counts.x * g.counts.y * stride, g.counts.z * stride);
}
// Normalized UV of an octahedral direction within a probe's tile, for a Sampled read. `inner` is the
// tile inner texel count (8 or 16). Returns UV in [0,1] over the WHOLE atlas.
float2 probe_tile_uv(ProbeGrid g, uint3 c, uint stride, uint inner, float3 dir)
{
    float2 oct = oct_encode(dir) * 0.5 + 0.5;      // [0,1]
    uint2 inner_origin = probe_tile_inner_origin(g, c, stride);
    float2 texel = float2(inner_origin) + oct * float(inner);   // sample the inner region
    float2 atlas = float2(probe_atlas_size(g, stride));
    return texel / atlas;
}

// --- DDGI irradiance sample ---------------------------------------------------------------------
// 8-probe trilinear blend of the base cell containing `world_pos`, each probe weighted by:
//   - trilinear cell weight
//   - backface / wrap-around weight (probes behind the surface normal contribute less)
//   - Chebyshev visibility (variance shadow) so a probe on the far side of a wall is rejected —
//     THE leak-preventing term (do not remove).
// Returns E/pi (dimensionally identical to the sky-SH sh_irradiance it replaces). `total_weight`
// out param: if ~0 the caller falls back to the sky-SH ambient (outside the volume / fully occluded).
float3 probe_irradiance(ProbeGrid g, Sampler2D irrad, Sampler2D vis,
                        float3 world_pos, float3 normal, float3 view_dir, out float total_weight)
{
    total_weight = 0.0;
    float3 sum = float3(0.0);

    // Bias the sample point off the surface (Majercik self-shadow bias). This must stay SMALL and
    // NORMAL-DOMINANT: it exists only to lift the query off the surface so the surface's own probe
    // isn't Chebyshev-rejected. RTXGI ships a fixed ~0.1 m normal bias + a small view bias, NOT a
    // fraction of the (metres-scale) grid spacing. The previous 0.75*spacing (~2.25 m) with a 4:1
    // view weight swung the query up to ~1.8 m along the CAMERA direction — which cell/probe won
    // flipped as the camera turned (the "swimming" artifact) and could shove the query through a
    // thin wall into a lit cell (a leak vector). Fixed small magnitudes, capped to a fraction of the
    // cell so tight grids stay safe.
    float min_sp = min(min(g.spacing.x, g.spacing.y), g.spacing.z);
    float normal_bias = min(0.25, 0.25 * min_sp);   // ~0.25 m off the surface (cap on tiny grids)
    float view_bias = min(0.10, 0.10 * min_sp);     // tiny view offset (glancing self-shadow)
    float3 bias = normal * normal_bias + view_dir * view_bias;
    float3 p = world_pos + bias;

    // Base cell.
    float3 grid_f = (p - g.origin) / g.spacing;
    int3 base = int3(floor(grid_f));
    float3 frac = grid_f - float3(base);

    for (uint i = 0; i < 8; ++i)
    {
        int3 offs = int3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
        int3 pc = base + offs;
        // Clamp to the grid (edge probes extend the volume slightly rather than dropping to zero).
        int3 cc = clamp(pc, int3(0), int3(g.counts) - 1);
        uint3 c = uint3(cc);
        // Inactive probes (classified inside geometry) never relight — their atlas texels are
        // stale/black; contributing them would darken every cell touching a wall. Skip entirely.
        if (g.active != nullptr && g.active.state[probe_linear(g, c)] == 0u) continue;
        float3 probe_pos = probe_world(g, c);

        // Trilinear weight.
        float3 tw = lerp(1.0 - frac, frac, float3(offs));
        float trilinear = tw.x * tw.y * tw.z;
        if (trilinear <= 0.0) continue;

        // Direction/distance are measured from the probe to the BIASED query point `p` — that is
        // exactly what the visibility atlas answers (distance from the probe outward along a
        // direction). Using the un-biased world_pos here would mismatch the query the atlas stores.
        float3 to_probe = probe_pos - p;
        float dist_to_probe = length(to_probe);
        float3 dir_to_probe = dist_to_probe > 1e-4 ? to_probe / dist_to_probe : normal;

        // Backface weight: probes in front of the surface contribute; smooth wrap (Majercik). The
        // small floor keeps a probe that is slightly behind the surface from going fully dark
        // (avoids over-dark corners) without letting a probe on the FAR side of a wall leak — that
        // rejection is the Chebyshev term's job. Kept small (0.05) so the wrap really dominates.
        float wrap = (dot(dir_to_probe, normal) + 1.0) * 0.5;
        float weight = trilinear * (wrap * wrap + 0.05);
        // Perf: a strongly back-facing corner with a small trilinear share contributes negligibly —
        // skip its two atlas fetches (visibility + irradiance). This roughly halves the average
        // fetch count (the ~half of the 8 cell corners behind the surface) with no visible change.
        if (weight < 1e-3) continue;

        // Chebyshev visibility (variance shadow) against the STATIC visibility atlas.
        float2 vuv = probe_tile_uv(g, c, 18, 16, -dir_to_probe);
        float2 moments = vis.SampleLevel(vuv, 0).rg;   // mean, mean^2 of distance
        float mean = moments.x;
        if (dist_to_probe > mean)
        {
            float variance = abs(moments.x * moments.x - moments.y);
            float d = dist_to_probe - mean;
            float cheb = variance / (variance + d * d);
            cheb = max(cheb * cheb * cheb, 0.0);        // sharpen (DDGI convention)
            weight *= cheb;
        }

        // A Chebyshev-rejected probe (on the far side of a wall) must contribute NOTHING — do not
        // floor the weight here (the previous 1e-6 floor let 8 rejected probes sum to a small stale
        // leak). The total-weight fallback below handles the "everything rejected" case cleanly.
        if (weight <= 0.0) continue;

        float2 iuv = probe_tile_uv(g, c, 10, 8, normal);
        float3 e = irrad.SampleLevel(iuv, 0).rgb;
        // Perceptual blend in sqrt space (DDGI: stored irradiance is sqrt-encoded for precision).
        sum += weight * sqrt(max(e, float3(0.0)));
        total_weight += weight;
    }

    if (total_weight <= 1e-5) return float3(0.0);
    float3 result = sum / total_weight;
    return result * result;   // undo the sqrt blend
}
