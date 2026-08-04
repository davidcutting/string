// Shared, PURE per-meshlet culling primitives (brief 04d M1). These functions are binding-free:
// every resource is passed as an argument (bindless slots are plain uints), so the module has no
// [[vk::binding]] / push-constant state of its own. It is imported by every cull consumer
// (meshlet_mesh, meshlet_shadow, meshlet_draw_cull) so there is ONE implementation of each test —
// the three inlined copies these replaced were a proven multi-file-bugfix hazard. Orchestration
// (ballots, payload packing, worklist compaction, stats) stays in the consumers, NOT here.
//
// All tests are the exact math the inlined copies used (verbatim extraction — M1 is a pure refactor
// gated at AE=0). Slang/HLSL matrix subscript m[i] IS row i (NOT a column as in GLSL); the frustum
// helpers rely on that. Reverse-Z throughout: min-reduced HiZ = FARTHEST scene depth.
import meshlet;

// Extract the 6 frustum planes from a view-projection (Gribb-Hartmann, ZERO_TO_ONE clip).
// m[i] IS row i in Slang/HLSL (MatrixLayoutColumn affects memory only, not subscript semantics).
void frustum_planes(float4x4 vp, out float4 planes[6])
{
    float4 r0 = vp[0];
    float4 r1 = vp[1];
    float4 r2 = vp[2];
    float4 r3 = vp[3];
    planes[0] = r3 + r0;  // left
    planes[1] = r3 - r0;  // right
    planes[2] = r3 + r1;  // bottom
    planes[3] = r3 - r1;  // top
    planes[4] = r2;       // near (reverse-Z: z>=0)
    planes[5] = r3 - r2;  // far
}

// Sphere-vs-frustum: keep if the sphere is inside (or straddling) all 6 planes.
bool sphere_in_frustum(float4 planes[6], float3 c, float r)
{
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float3 n = planes[i].xyz;
        float len = length(n);
        if (len < 1e-6) continue;
        if ((dot(n, c) + planes[i].w) / len < -r) return false;
    }
    return true;
}

// Backface cone cull (meshopt CENTER-based reference test): returns TRUE if the whole meshlet
// faces away from `eye` and can be discarded. cone_axis must already be WORLD-space & normalized.
//   cull if dot(center - eye, axis) >= cutoff * length(center - eye) + radius
// The +radius term keeps it conservative when using the sphere center (not the cone apex) — without
// it, thin geometry at glancing angles is falsely culled. Callers guard cone_cutoff < 1 / two-sided.
bool cone_backfacing(float3 axis_ws, float cone_cutoff, float3 center_ws, float radius_ws, float3 eye)
{
    float3 to_center = center_ws - eye;
    return dot(to_center, axis_ws) >= cone_cutoff * length(to_center) + radius_ws;
}

// Project a world-space bounding sphere to a screen-space rect + CONSERVATIVE nearest depth, then
// test against the HiZ pyramid (reverse-Z: min-reduced = farthest). Returns true if potentially
// visible. Conservatism is mandatory: a false "hidden" here is a visible hole in the scene.
//
// Binding-free: the pyramid sampler array + its slot are arguments (`tex` / `slot`); `cull_vp` is the
// projection the pyramid was built with, `eye` the pyramid's camera, `dims`/`mips` the mip-0 size and
// chain length. hiz_mips==0 means "HiZ off / not built" -> always visible.
bool hiz_visible(Sampler2D tex[], uint slot, uint2 dims, uint mips, float4x4 cull_vp, float3 eye,
                 float3 center_ws, float radius)
{
    if (mips == 0) return true;   // HiZ off / not yet built -> everything passes

    // Inflate the test sphere a little: the min-reduced half-res pyramid + conservative-rect sampling
    // still leaves ~1px of silhouette-edge over-cull. Padding keeps those edge meshlets visible.
    float r = radius * 1.2;

    // Nearest point of the sphere toward the camera — the sphere's closest (largest, reverse-Z) depth.
    // Using the CENTER depth would cull any meshlet whose front half pokes out from behind an occluder.
    float3 to_c = center_ws - eye;
    float dist = length(to_c);
    if (dist <= r * 1.05) return true;   // camera inside (or grazing) the sphere — keep
    float4 clip_near = mul(cull_vp, float4(center_ws - to_c * (r / dist), 1.0));
    if (clip_near.w <= 1e-4) return true;     // reaches the camera plane — keep
    float nearest = clip_near.z / clip_near.w;

    float4 clip = mul(cull_vp, float4(center_ws, 1.0));
    if (clip.w <= 0.0) return true;
    float2 uv = (clip.xy / clip.w) * 0.5 + 0.5;

    // Screen-space half-extents: radius scaled by the projection diagonals (|VP row .xyz| = P00/P11
    // since the view rotation is norm-preserving) over view depth (= clip.w). NDC halfwidth -> uv.
    float2 sr = r * float2(length(cull_vp[0].xyz), length(cull_vp[1].xyz))
              / max(clip.w, 1e-4) * 0.5;
    // Rect touches the screen border -> keep (off-screen depth is unknown; clamping tests edge texels
    // that don't cover the sphere's visible sliver — an edge-holes bug).
    if (any(uv - sr < 0.0) || any(uv + sr > 1.0)) return true;
    float2 uv_lo = clamp(uv - sr, 0.0, 1.0);
    float2 uv_hi = clamp(uv + sr, 0.0, 1.0);

    // Pick a mip where the rect spans at MOST ~4x4 texels, then sample every texel of that footprint
    // and take the FARTHEST (min). A coarser mip whose min-reduce already folds the footprint's
    // farthest into few texels, fully covered here, is conservative.
    float rect_px = max((uv_hi.x - uv_lo.x) * float(dims.x),
                        (uv_hi.y - uv_lo.y) * float(dims.y));
    uint mip = uint(clamp(ceil(log2(max(rect_px * 0.25, 1.0))), 0.0, float(mips - 1)));
    float occ = 1.0;
    [unroll] for (int y = 0; y <= 4; ++y)
    [unroll] for (int x = 0; x <= 4; ++x)
    {
        float2 uv_s = lerp(uv_lo, uv_hi, float2(x, y) * 0.25);
        occ = min(occ, tex[slot].SampleLevel(uv_s, float(mip)).r);
    }
    // reverse-Z: smaller depth = farther. Hidden only if our NEAREST point is behind the farthest
    // occluder across the whole rect. Small guard band absorbs half-res/precision slop at edges.
    return nearest >= occ - max(occ * 1e-3, 1e-5);
}

// Screen-space-error LOD select (verbatim port of the CPU/draw-cull math). Picks the COARSEST LOD
// whose stored simplification error projects below `error_px`. `lods` is the draw's per-LOD table,
// `lod_count` its length; returns a LOD index in [0, lod_count). focal = half_h / tan(fov/2).
uint select_lod(MeshletLod lods[4], uint lod_count, float3 center_ws, float3 eye, float focal,
                float error_px)
{
    if (lod_count <= 1) return 0;
    float dist = max(length(center_ws - eye), 0.01);
    float px_per_unit = focal / dist;
    uint pick = 0;
    for (uint l = 0; l < lod_count; ++l)
        if (lods[l].error * px_per_unit <= error_px) pick = l;
    return min(pick, lod_count - 1);
}
