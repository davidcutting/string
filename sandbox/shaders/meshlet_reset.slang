// Per-frame reset of the GPU mesh stats counters (brief 03). Cleared each frame before the two-pass
// task/mesh dispatch accumulates into them. The visibility bitfield is NOT reset here (it persists
// across frames — pass 2 updates it), except on an explicit teleport clear handled CPU-side.
struct Push { uint *stats; uint stats_words; };
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(64)]
void reset_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < pc.stats_words) pc.stats[id.x] = 0u;
}
