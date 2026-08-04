// HiZ depth-pyramid downsample (brief 03). Reduces a source mip (the depth buffer for mip 0, then
// each pyramid level) into the next R32F mip with a MIN reduce — reverse-Z, so min = FARTHEST scene
// depth in the footprint. Conservative 2x2 gather; edge texels clamp. One dispatch per output mip.
struct Push
{
    uint src_slot;    // bindless slot of the source (mip N)   — sampled
    uint dst_slot;    // bindless slot of the destination (mip N+1) — storage image
    uint2 dst_size;   // destination mip dimensions (px)
    uint2 src_size;   // source mip dimensions (px)
    uint copy_depth;  // 1 = mip0 copy from the scene depth (Sampler2D), 0 = pyramid->pyramid
    uint src_level;   // mip level of src_slot to sample (pyramid->pyramid: the level being reduced)
};
[[vk::push_constant]] Push pc;

[[vk::binding(1, 0)]] Sampler2D src_textures[];        // sampled depth / pyramid mips
[[vk::binding(2, 0)]] RWTexture2D<float> dst_images[]; // storage pyramid mips

[shader("compute")]
[numthreads(8, 8, 1)]
void build_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;

    float reduced;
    if (pc.copy_depth == 1)
    {
        // Mip0 copy from the (non-pow2) scene depth. The destination must be a UNIFORM STRETCH of
        // the source: the cull test samples the pyramid with SCREEN uvs, so mip0's [0,1] uv must
        // cover the source's full [0,1] uv. (A texel-space id*2 mapping here squeezed the whole
        // screen into the top-left of the pyramid and left clamp-junk elsewhere — every HiZ lookup
        // then tested the wrong screen location.) Sample a 3x3 spread across this texel's source
        // footprint; over-coverage only makes the min (farthest) more conservative.
        float2 scale = float2(pc.src_size) / float2(pc.dst_size);
        float2 texel = 1.0 / float2(pc.src_size);
        float2 base = float2(id.xy) * scale;
        reduced = 1.0;
        [unroll] for (int y = 0; y < 3; ++y)
        [unroll] for (int x = 0; x < 3; ++x)
        {
            float2 px = min(base + float2(x, y) * (scale * 0.5), float2(pc.src_size) - 0.5);
            reduced = min(reduced, src_textures[pc.src_slot].SampleLevel((px + 0.25) * texel, 0).r);
        }
    }
    else
    {
        // Pyramid->pyramid: exact 2x2 gather (pow2 chain; clamp covers the 1px tail levels).
        int2 s = int2(id.xy) * 2;
        int2 mx = int2(pc.src_size) - 1;
        float2 texel = 1.0 / float2(pc.src_size);
        float2 uv00 = (float2(min(s + int2(0,0), mx)) + 0.5) * texel;
        float2 uv10 = (float2(min(s + int2(1,0), mx)) + 0.5) * texel;
        float2 uv01 = (float2(min(s + int2(0,1), mx)) + 0.5) * texel;
        float2 uv11 = (float2(min(s + int2(1,1), mx)) + 0.5) * texel;
        // MUST sample the SOURCE mip (src_level), not level 0. src_slot is the whole-chain view; a
        // hardcoded level-0 sample read mip0 for EVERY level with the current level's coarser grid,
        // so each coarse mip min-reduced only a SPARSE subset of mip0 texels and MISSED the farthest
        // ones between them — coarse `occ` came back too NEAR, over-culling visible geometry (worse
        // the more mips there are: the >800px / 11-mip resolution-dependent HiZ over-cull bug).
        float lvl = float(pc.src_level);
        float d0 = src_textures[pc.src_slot].SampleLevel(uv00, lvl).r;
        float d1 = src_textures[pc.src_slot].SampleLevel(uv10, lvl).r;
        float d2 = src_textures[pc.src_slot].SampleLevel(uv01, lvl).r;
        float d3 = src_textures[pc.src_slot].SampleLevel(uv11, lvl).r;
        // Reverse-Z: farthest = smallest depth value. Min reduce.
        reduced = min(min(d0, d1), min(d2, d3));
    }

    dst_images[pc.dst_slot][int2(id.xy)] = reduced;
}
