// Brief 04c (resolution b) — EXPANSION/COMPACTION PHASE of the two-phase GPU cull. Turns the DRAW
// phase's per-draw meshlet counts (stable slot == draw index) into a DENSE, ORDER-STABLE array of
// per-draw VkDrawMeshTasksIndirectCommandEXT commands + a parallel DrawRecord {draw_index, lod}
// array, plus the surviving-draw count for vkCmdDrawMeshTasksIndirectCountEXT.
//
// WHY compacted per-draw commands (not a single mesh-only dispatch): a single vkCmdDrawMeshTasks
// dispatch has NO rasterization-order guarantee between mesh workgroups on RADV/RDNA3, so coplanar
// depth-tie winners re-roll per frame (visible floor shimmer, run-twice AE ~0.1%). Multi-command
// vkCmdDrawMeshTasksIndirectCountEXT IS ordered by command index per spec — the command boundaries
// between draws pin the coplanar winners (measured AE=0). This scan is the COMPACTOR: it removes the
// empty per-draw slots (the fan-out cost the brief targets) while keeping one command per surviving
// draw, in ASCENDING DRAW INDEX ORDER (the scan preserves input order — no atomics in the ordering
// path, so submission order is deterministic: the 03b z-fighting constraint).
//
// Two-pass block scan of the per-draw SURVIVAL PREDICATE (counts[d] != 0 -> 1, else 0):
//   scan_blocks : one workgroup (kScanBlock lanes) per block -> each draw's block-local exclusive
//                 compact index into offsets[], the block's surviving-draw total into block_sums[].
//   scan_carry  : a single workgroup exclusive-scans block_sums[] IN PLACE -> block base indices,
//                 and writes the grand total (surviving draw count) -> count.
//   fill        : one thread per draw; surviving draws write their command + record at the compact
//                 slot base = block_base + block-local index. commands[s]={ceil(mcount/32),1,1};
//                 records[s]={d, draw_lod[d]}.
// Handles >=16k draws: kScanBlock=256 -> <=64 blocks at 16384 draws, scanned by one 256-lane pass.
import meshlet;

static const uint kTaskGroup = 32;   // meshlets tested per task workgroup (matches meshlet_mesh.slang)

struct Push
{
    UintBuffer *counts;             // in : per-draw meshlet count (stable slot d), 0 = culled/skipped
    UintBuffer *offsets;            // scratch: per-draw block-local compact index (scan_blocks->fill)
    UintBuffer *block_sums;         // scratch: per-block surviving total (scan_blocks) -> base (scan_carry)
    UintBuffer *draw_lod;           // in : per-draw selected LOD (fill copies it into records[])
    MeshTaskCommandBuffer *commands;// out: DENSE per-surviving-draw indirect commands {ceil(mcount/32),1,1}
    DrawRecordBuffer *records;      // out: DENSE parallel {draw_index, lod}
    CountBuffer *count;             // out: surviving-draw count (indirect draw count)
    uint draw_count;                // active_draw_count_
    uint block_count;               // ceil(draw_count / kScanBlock)
    uint max_draws;                 // commands[]/records[] capacity (overflow guard == cull_max_draws_)
    uint _pad;
};
[[vk::push_constant]] Push pc;

groupshared uint g_scan[kScanBlock];

// Pass A: each workgroup scans one block of the per-draw survival predicate into a block-local
// exclusive compact index (offsets[]) and publishes the block's surviving-draw total (block_sums[]).
// Hillis-Steele inclusive scan then shift to exclusive — deterministic, order-preserving.
[shader("compute")]
[numthreads(kScanBlock)]
void scan_blocks(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    uint block = gid.x;
    uint d = block * kScanBlock + tid;
    uint p = (d < pc.draw_count && pc.counts.v[d] != 0u) ? 1u : 0u;   // survival predicate
    g_scan[tid] = p;
    GroupMemoryBarrierWithGroupSync();

    // Inclusive Hillis-Steele scan.
    for (uint off = 1u; off < kScanBlock; off <<= 1u)
    {
        uint add = (tid >= off) ? g_scan[tid - off] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[tid] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    // Exclusive = inclusive - own predicate.
    if (d < pc.draw_count) pc.offsets.v[d] = g_scan[tid] - p;
    // Last active lane publishes the block total (== inclusive scan of the last element).
    if (tid == kScanBlock - 1u) pc.block_sums.v[block] = g_scan[tid];
}

groupshared uint g_carry[1024];

// Pass B: a single workgroup exclusive-scans block_sums[] in place (block base compact indices) and
// writes the grand total (surviving-draw count) to count. block_count <= 1024 (16384 draws / 256).
[shader("compute")]
[numthreads(1024)]
void scan_carry(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    uint v = (tid < pc.block_count) ? pc.block_sums.v[tid] : 0u;
    g_carry[tid] = v;
    GroupMemoryBarrierWithGroupSync();

    for (uint off = 1u; off < 1024u; off <<= 1u)
    {
        uint add = (tid >= off) ? g_carry[tid - off] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_carry[tid] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    uint total = g_carry[min(pc.block_count, 1024u) - 1u];   // inclusive total across all blocks
    if (tid < pc.block_count) pc.block_sums.v[tid] = g_carry[tid] - v;   // exclusive block base

    // Surviving-draw count for vkCmdDrawMeshTasksIndirectCountEXT.
    if (tid == 0u) pc.count.count = min(total, pc.max_draws);
}

// Pass C: one thread per draw. Surviving draws write their compacted command + record at
// base = block_base + block-local compact index. commands/records stay in ascending draw order.
[shader("compute")]
[numthreads(64)]
void fill(uint3 id : SV_DispatchThreadID)
{
    uint d = id.x;
    if (d >= pc.draw_count) return;
    uint mcount = pc.counts.v[d];
    if (mcount == 0u) return;   // culled/skipped draws are compacted OUT (no empty command slot)
    uint slot = pc.block_sums.v[d / kScanBlock] + pc.offsets.v[d];
    if (slot >= pc.max_draws) return;   // overflow guard (sized to worst case; assert CPU-side)

    MeshTaskCommand cmd;
    cmd.x = (mcount + kTaskGroup - 1u) / kTaskGroup;   // ceil(meshlet_count / kTaskGroup)
    cmd.y = 1u;
    cmd.z = 1u;
    pc.commands.cmds[slot] = cmd;

    DrawRecord rec;
    rec.draw_index = d;
    rec.lod = pc.draw_lod.v[d];
    pc.records.records[slot] = rec;
}
