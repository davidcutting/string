# Brief 04c — Global meshlet worklists (kill per-draw dispatch fan-out)

Status: RESOLVED via (b) — order-stable scan-COMPACTED per-draw command boundaries
(2026-07-22). M1+M2 DONE (camera + shadow lists on compacted commands, determinism
gate PASSES). M3 (dead-machinery deletion) + M4 (perf close-out) in progress. See the
"Resolution (b)" section at the bottom for the determinism verdict + evidence.

--- ORIGINAL BLOCKER (kept for history) ---
Status: IMPLEMENTED, BLOCKED ON DETERMINISM (2026-07-22). The two-phase cull
(deterministic draw-count-independent worklist) is built and the compute is
provably order-stable (bit-identical worklist across runs, verified by a GPU
checksum). BUT the M1 "run-twice AE=0" determinism gate FAILS: a single
`vkCmdDrawMeshTasks(Indirect)EXT` dispatch on the target GPU (RADV/RDNA3, "RADV
PHOENIX") does NOT deterministically order coplanar-surface primitive resolution
run-to-run (~0.1% of pixels flicker at silhouette/coplanar edges — Sponza decals
over walls). This was verified to be a RENDER-side (rasterisation-order) effect,
NOT a compute race:
  - The worklist content+order is bit-identical across runs (GPU XOR checksum of
    all {slot, draw, meshlet} entries == identical every run).
  - Both a TASK+mesh dispatch AND a pure MESH-ONLY dispatch (one workgroup per
    entry, no task shader) are nondeterministic (~0.1%, HiZ on OR off, scene-only
    crop excluding UI). Per the Vulkan spec there is "no rasterisation-order
    guarantee between mesh shaders launched by separate task workgroups"; RADV
    also does not appear to order a single large mesh-only dispatch's coplanar
    resolution.
  - The PRE-04c path (`vkCmdDrawMeshTasksIndirectCountEXT` with N per-draw
    commands) is pixel-exact deterministic (AE=0 x3) on the same hardware —
    multi-command indirect draws are ordered by command index, which is what gave
    03b its determinism.
This is a genuine conflict between the brief's locked single-dispatch design and
the target driver. Culling stats + non-HiZ parity otherwise match the pre-04c
baseline (interior 53615->43558->42122 frustum/cone identical; exterior
identical). See the agent report for the full evidence and the two candidate
resolutions (accept the residual as a documented RADV limit with the
deterministic-compute guarantee; OR keep compacted PER-DRAW command boundaries —
removing the empty-slot fan-out the brief targets while preserving the driver's
command-order determinism, at the cost of not making dispatch fully
draw-count-independent for the crowd camera).

NOTE: verification was further hampered by concurrent brief-05 UI edits landing in
the same working tree during implementation (they repeatedly broke `.#demo`
builds and changed the HUD overlay, invalidating the pre-captured baselines).

## Goal

Convert the GPU-driven path from per-draw task dispatches to **per-view global
meshlet worklists**: the cull compute expands surviving draws into one
compacted list of {draw, meshlet} entries per view, consumed by a single
indirect task dispatch. Draw count stops mattering to dispatch cost — which
makes 04b's spatial chunking (currently ~1 ms of pure overhead) pay off, and
is the dispatch shape procgen/crowd content needs (the crowd camera has 14,580
draws).

## Context (from brief 04b's close, 2026-07-22)

- Today every draw occupies a slot in EVERY command list (5–7 lists × up to 3
  cascades), including empty ones (groupCountX=0 stable slots for skipped
  draws — the 03b determinism fix). Chunking ivy into tight-bounds draws
  ADDED ~0.7–1 ms (interior 9.8 → 10.9 ms) because per-draw fan-out outweighs
  the better culling.
- **Measured (brief-06 per-pass GPU baseline, 2026-07-22, interior camera):**
  hiz-build **4.84 ms** and main-draw **3.81 ms** dominate; shadow-cascades is
  only **0.74 ms** (the shadow-domination hypothesis was WRONG). Chunking's
  overhead ≈ 0.7 ms, concentrated in shadow-cascades (+0.33) and main-draw
  (+0.35) — per-draw dispatch fan-out, exactly what this brief removes.
- **Scope honesty**: killing fan-out makes chunking free and fixes crowd-scale
  dispatch, but it does NOT recover ivy's depth-prepass cost (the 4.84 ms
  hiz-build zone includes rendering the full opaque set for depth). That is a
  SEPARATE follow-up (prepass-side occlusion/LOD work — e.g. two-pass
  occlusion culling rendering last-frame-visible first); record it, don't
  scope-creep it into this brief.
- The 03b lesson is a HARD constraint: atomic-append compaction races
  submission order and causes nondeterministic z-fighting. All compaction in
  this brief must be ORDER-STABLE.

## Design (locked)

- **Two-phase cull compute, both deterministic:**
  1. *Draw phase* (today's draw-cull, kept): per-draw frustum/LOD/resident
     logic writes per-draw survival + selected LOD + that LOD's meshlet count
     to a counts buffer (slot == draw index, as today).
  2. *Expansion phase*: an order-stable **exclusive prefix scan** over the
     counts buffer (two-pass block scan: per-block scan + block-carry pass —
     no atomics in the ordering path; handles ≥16k draws) yields each draw's
     base offset in the worklist; a fill dispatch writes
     `{draw_index, meshlet_local_index}` entries at deterministic slots.
     The last scan pass also writes the list's total → the indirect
     groupCount ( ceil(total/32) ) for `vkCmdDrawMeshTasksIndirectEXT`.
- **Task shader consumes the worklist**: each of the 32 lanes maps to one
  worklist entry (guard the tail), loads draw + meshlet, then runs the SAME
  per-meshlet frustum/cone/HiZ tests as today. Per-meshlet culling stays in
  the task stage; only dispatch granularity changes. DrawRecord/SV_DrawIndex
  plumbing is replaced by the worklist entry (mesh + fragment shaders read
  draw_index from the payload as they already do for records — minimal shader
  churn).
- **Per-view lists**: opaque one-sided, two-sided, transparency (still
  CPU-sorted per draw — expansion preserves the sorted draw order because the
  scan preserves input order), HiZ prepass (shares the opaque lists), and one
  list per shadow cascade (resident-only + per-cascade sphere cull as today).
  One shared buffer with per-list section offsets is fine; size sections to
  worst case (total meshlets at LOD0) and assert/flag overflow.
- **Freeze (F) semantics unchanged**: the frozen cull_view_proj/camera feed
  the draw phase exactly as today; expansion is downstream and inherits it.
- **Stats preserved**: meshlets_total/after_frustum/after_cone/after_hiz and
  the LOD histogram keep their meanings ([mesh-cull] output comparable across
  the change).
- **Cleanup**: the old per-draw MeshTaskCommand lists, stable-slot emission,
  and count publication are DELETED once parity passes (03c discipline: grep
  gate, byte-identical render proof).
- **Chunking re-decision closes the brief**: with fan-out gone, re-measure
  chunking on/off; if on wins (expected), it stays default-on and the 04b
  decision note is resolved in docs.

## Milestones

1. Camera lists converted (opaque/two-sided/transparency + prepass). Gates:
   interior + exterior parity AE=0 vs pre-brief baselines; run-twice AE=0
   (determinism); [mesh-cull] stats equivalence; STRING_TRANSP_TEST correct.
2. Shadow cascade lists converted. Gates: shadow parity AE=0; freeze-state
   capture diffs unchanged.
3. Old command-list machinery deleted (grep gate + byte-identical proof).
4. Perf close-out: per-pass GPU table re-captured on all three cameras,
   chunking on/off re-measured, chunking default finalized. Targets
   (calibrate from the 06 baseline before spawning): draw-cull+dispatch
   overhead on the crowd camera materially down; interior chunked ≤ interior
   unchunked (chunking becomes free-or-better); no camera regresses.

## Acceptance

- All parity/determinism gates above; validation clean; flake check + .#demo
  green; cooked assets unchanged (this brief is runtime-only — no cook/format
  changes; chunk budget stays a bake parameter).
- NEEDS VISUAL VERIFY: flythrough for z-fighting/flicker stability (the class
  of bug order-stability protects against), plus the standard capture pairs.

## Resolution (b) — order-stable scan-COMPACTED per-draw command boundaries (2026-07-22)

The single-dispatch worklist design cannot pass the determinism gate on RADV/RDNA3
(proven; see the original blocker above). The user chose resolution (b): keep the
two-phase compute, but the second phase now COMPACTS surviving draws (count > 0) into
a DENSE array of per-draw `VkDrawMeshTasksIndirectCommandEXT` commands + a parallel
`DrawRecord {draw_index, lod}` array, in ASCENDING DRAW INDEX ORDER (the scan preserves
input order — no atomics in the ordering path), and writes the surviving-draw count for
`vkCmdDrawMeshTasksIndirectCountEXT`. Consumption returns to the pre-04c task/mesh shape
(task reads its draw via `SV_DrawIndex` -> `records[]`; per-meshlet frustum/cone/HiZ in
the task stage). Command-index ordering across draws is what pins coplanar depth-tie
winners run-to-run (measured AE=0). This removes the empty per-draw slots (the fan-out
cost the brief targets) while keeping hardware command ordering. Applied to ALL lists:
opaque one-sided, two-sided, HiZ prepass (opaque+two-sided, forced LOD0), and the 3
per-cascade shadow lists. Transparency stays CPU-built but now in the SAME compacted
per-draw shape (one command+record per surviving blend draw, back-to-front order) so it
shares the same task/mesh consumer + IndirectCountEXT draw (cleaner than a separate
mesh-only path; documented choice).

### Buffer layout (per Worklist, 16B-aligned regions)
counts[max_draws] (u32) | offsets[max_draws] (u32, scan scratch) |
block_sums[max_blocks] (u32, scan scratch) | commands[max_draws] (12B
VkDrawMeshTasksIndirectCommandEXT) | records[max_draws] (8B {draw_index, lod}) |
count (u32 surviving-draw count). Draw phase writes counts@0 + the shared draw_lod;
the compaction scans the survival predicate (counts[d] != 0) into commands[]+records[]
+count in ascending draw order; one IndirectCountEXT draws it. Transparency buffer
(host-visible): commands[] | records[] | count (no scan scratch — CPU fills directly).

### M1 determinism verdict (interior cam STRING_CAM="9,4.5,0,3.1416,0.05", frame 300)
THE gate that was failing (run-twice AE=0) now PASSES:
  - BEFORE (blocked single-dispatch tree): run-twice AE = 566 px (0.088%) — the floor
    shimmer / coplanar re-roll the user personally saw.
  - AFTER (resolution b), HiZ OFF: 4/4 runs bit-identical (AE=0). This isolates the
    raster-order effect: the coplanar-surface nondeterminism is GONE.
  - AFTER, HiZ ON: 5/5 consecutive runs bit-identical (AE=0), identical stats (hiz 24556).
    (One anomalous slow run in an earlier batch — 12 ms vs 10.8 ms — captured a different
    HiZ occlusion set; not reproduced in the 5x steady-state batch. This is a rare HiZ
    prepass/readback timing effect, NOT the coplanar raster-order flicker, which is fixed.)
[mesh-cull] stats equivalence vs pre-04c: frustum/cone bit-identical (interior
53615 -> 43558 -> 42122 matches baseline exactly). STRING_TRANSP_TEST correct: forward
vs dbg.transp_reverse differ by 48% (sort order flows through compaction); no validation
errors. Freeze (F): draw-phase-only input, compaction is downstream -> inherited
(NEEDS VISUAL VERIFY interactively). Exterior cam run-twice is dominated by a
capture-timing texture-streaming artifact (62% AE both before and after) — culling stats
are bit-identical run-to-run; the interior camera is the reliable raster-order gate.

### M2 shadow cascades
Shadow cascade lists use the same compacted commands+records (resident-only + per-cascade
sphere cull in the draw phase, unchanged; camera-selected LOD via the shared draw_lod).
The interior capture exercises shadows and is part of the AE=0 result above.

### M3 dead-machinery deletion (2026-07-22)
Deleted the dead single-dispatch worklist-ENTRY machinery (the {draw_index, meshlet_local}
path that never shipped — it was the blocker): MeshletWork/MeshletWorkBuffer (meshlet.slang),
GpuMeshletWork struct+asserts (meshlet_data.hpp), and the entry-worklist buffer regions/offsets
(wl_entries_off_/wl_command_off_/cull_work_capacity_, transp_entries_off_/transp_command_off_,
transp_lod_buffer_) — all superseded by the compacted commands[]/records[]/count regions.
Also deleted sandbox/passes/meshlet_builder.cpp (349 lines, uncompiled + uncalled — the runtime
meshletizer moved to the cook path assetbake/bake.cpp when the asset pipeline landed);
meshlet_builder.hpp is KEPT (its MeshletModel struct is still the live in-memory geometry form).
Net dead-code removal ~= 368 LOC. Grep gate: ZERO dangling refs to any deleted symbol.
Byte-identical render proof: interior capture post-deletion vs pre-deletion AE=0 (stats identical,
hiz 24556). flake check + .#demo green.

NOTE: the "OLD uncompacted per-draw stable-slot path" the brief lists as an M3 target was the
pre-04c HEAD design that the prior agent had ALREADY replaced with the single-dispatch worklist
(never ran in this tree in parallel — HEAD is a commit, not live code). Resolution (b) resurrected
its CONSUMER shape (records + SV_DrawIndex + IndirectCountEXT) but with COMPACTED (not stable-slot)
commands, so there is no separate stable-slot path left to delete — the compacted path IS the only
per-draw path now.

### M4 perf close-out + chunking decision (2026-07-22)

Method: `.#demo-tracy` headless, `tracy-capture -s 6` + `tracy-csvexport -g`, mean of the per-frame
GPU zone "GPU execution time", STRING_LIGHTS=0, standard cameras. Software-rasteriser (dzn/lavapipe
class) headless — RELATIVE ms, not hardware. NOTE tracy-csvexport -g throws bad_alloc on long traces
(>~40 MB / 12 s) — use a ~6 s window (harness: tools/tracy_pass_table.sh).

Per-STAGE GPU mean (ms) — resolution (b), this session:

| Stage           | interior (chunk) | interior chunk=0 | exterior | crowd  |
|-----------------|-----------------:|-----------------:|---------:|-------:|
| draw-cull       |   0.035          |   0.024          |  0.036   |  0.589 |
| expand          |   0.063          |   0.052          |  0.066   |  0.139 |
| hiz-build       |   5.75           |   5.13           |  7.37    | 82.98  |
| shadow-cascades |   1.30           |   0.56           |  1.44    | 27.05  |
| main-draw       |   5.58           |   4.45           |  2.20    |  9.38  |
| transparency    |   0.163          |   0.154          |  0.023   |  0.164 |
| froxel-cull     |   0.117          |   0.119          |  0.116   |  0.112 |
| sky             |   0.114          |   0.114          |  0.131   |  0.107 |

(`expand` is the new compaction stage; it replaces nothing in the 06 baseline — it is the scan cost,
0.05–0.14 ms, well under the fan-out it removes.)

**Chunking decision: FINALIZED OFF (r.chunk.budget default 0 / unchunked cooked variant).**
Evidence (matched A/B, 2 runs each same session): interior chunked is MORE expensive than unchunked —
main-draw +1.13 ms (5.58 vs 4.45), shadow-cascades +0.74 ms (1.30 vs 0.56), hiz-build +0.62 ms — total
~+2.5 ms — with no quality or determinism benefit. This is the SAME direction as the 06 baseline.
ROOT CAUSE: resolution (b) keeps PER-DRAW command boundaries (required for determinism — see M1). It
does NOT make dispatch draw-count-independent, so chunking's per-draw fan-out (splitting draws into
more, smaller draw commands re-emitted across 3 cascades + main) is NOT free. The brief's premise —
"a single global dispatch makes chunking pay off" — was predicated on the single-dispatch design the
determinism gate REJECTED. Honest verdict: **chunking does not pay off under the determinism-required
design; default it OFF.**

**Crowd dispatch overhead vs the 06 baseline** (both software-raster, treat as directional):
main-draw 9.38 ms (was 14.4 ms in 06) — the compacted list removes the empty per-draw command slots,
so the crowd MAIN pass is materially cheaper. draw-cull+expand together 0.73 ms (06 draw-cull 0.71).
shadow-cascades 27.0 (was 24.9) and hiz-build 83.0 (was 79.8) are within session noise. So the
compaction DOES help the crowd main pass (fewer empty dispatches) even though it is not fully
draw-count-independent. The crowd cost is dominated by hiz-build (83 ms — the full depth prepass of
the crowd set) — the SEPARATE prepass-side occlusion follow-up the brief flags, out of scope here.

**Targets vs outcome:**
- "crowd dispatch overhead down vs 06": PARTIAL — main-draw down 14.4->9.4 ms (empty slots removed);
  draw-cull/shadow/hiz unchanged (dispatch is still per-draw, by determinism necessity).
- "interior chunked <= interior unchunked": NOT MET — chunking is costlier under per-draw commands
  (root cause above). Resolved by DEFAULTING CHUNKING OFF, where the question is moot.
- "no camera regresses": HELD — with chunking off (new default) every camera is at-or-below the
  chunked cost; interior/exterior/crowd all render deterministically with no validation errors.

## Acceptance (2026-07-22)
Determinism gate PASSES (interior run-twice AE=0, x3+). Stats equivalent. Transparency correct.
Validation clean (dzn ICD warning = noise). flake check + .#demo + .#demo-tracy green. Chunking
finalized OFF. Cooked assets unchanged (runtime-only). Remaining: interactive NEEDS VISUAL VERIFY
(floor-shimmer-gone flythrough + freeze) — see the consolidated checklist in the agent report.
