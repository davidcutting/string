# Brief 04e — Render graph maturation: execution, aliasing, scratch, queues

Status: DONE (2026-07-23, user visual verify passed — flythrough clean at
defaults, STRING_ASYNC=1). M1–M5 implemented and gated (parity AE=0, run-twice,
motion, sync validation — see running log). One honest miss: the M4 Tracy
capture shows NO actual multi-queue overlap on this machine — the demo's
acquire-before-record pacing serializes the CPU to ~1 frame ahead, so the async
chain lands in the inter-frame GPU idle gap. NOT a graph-execution bug: the
derived timeline wait is stage-limited (FRAGMENT), sync validation shows no
extra edges, and zero overlap reproduces even in the 28 ms GPU-bound crowd
scene — the async submit simply arrives after frame N-1 already drained.
Perf win blocked on a pacing follow-up (late-latch the swapchain acquire /
restore CPU run-ahead); details in the M4 gate section. Running log at the end
of this file.

## Goal

Finish the render graph: the existing planner (`render_graph.hpp` — adjacency,
toposort, resource lifetimes, built from the SAME ResourceUsage the executable
passes declare) currently feeds NOTHING. Connect it to execution so declared
dependencies drive barriers, transient memory is aliased from lifetime
analysis, per-frame scratch is a bump allocation, and work is scheduled onto
the queues the hardware ACTUALLY exposes. After this brief, every future pass
(VFX, post, UI) inherits correct minimal sync by declaration instead of
hand-rolled barriers.

## Context

- Execution today: renderer calls passes in authored order on the graphics
  queue only; barriers are hand-placed and BROAD (brief-02 history: fixed by
  widening stage masks). The device creates a compute queue that carries no
  frame work. The geometry pass hand-sequences its internal stages.
- The planner's comment says it itself: "connecting the two is a later
  stage." This brief is that stage.
- Decisions locked with user (2026-07-22): aliasing + scratch + hardware-true
  queue sizing are in scope; transparent queue virtualization is NOT (see
  guardrail below).

## Decisions (locked)

- **M1 — Queue/command infrastructure (no behavior change):**
  - Device enumerates a **capability table**: queue families, per-family
    queue counts, flags (graphics/compute/transfer/sparse/present),
    timestamp validity. Create the full useful queue set once at init
    (e.g. RDNA3: 1 gfx + up to 4 compute + transfer).
  - App/engine-facing API stays LOGICAL: named submission lanes ("main",
    "async-compute-N", "transfer"). **Guardrail: no transparent
    multiplexing** — a logical lane maps 1:1 to a real queue chosen at init;
    if hardware has fewer queues, fewer lanes EXIST (capability table says
    so) and the graph's placement policy degrades explicitly. Pooling
    decisions live in ONE place (the graph scheduler), never behind
    individual submits — silent multiplexing creates hardware-dependent
    serialization/priority bugs.
  - Command abstraction: per-queue, per-frame-slot command pools; recorder
    acquisition tied to (lane, frame); centralized timeline-semaphore
    management for cross-lane edges. (Multithreaded recording is explicitly
    OUT of scope — flag the seam, don't build it.)
  - Tracy: one GPU context per queue so multi-queue overlap is VISIBLE in
    traces (extend the existing per-pass zone wiring).
- **M2 — Graph-driven execution + derived barriers (the correctness core):**
  - Passes' declared ResourceUsages feed the planner every frame (or once,
    with dynamic bits parameterized); execution follows the toposort;
    barriers are DERIVED per-resource with minimal stage/access scopes.
  - The existing hand-placed barriers become the validation baseline: derived
    sync must reproduce byte-identical output (AE=0 on all standard
    captures) with validation layers + sync-validation (VK_LAYER: enable
    `VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` for this
    milestone's test runs) clean. Then the hand barriers are DELETED (03c
    discipline: grep gate).
  - Intra-pass stages (geometry pass's shadow/HiZ/froxel/cull sequence)
    either become graph nodes or keep local barriers — agent judgment, but
    the frame-level graph must at minimum cover all INTER-pass and
    inter-queue sync.
- **M3 — Transient aliasing + scratch (the memory payoff):**
  - Lifetime analysis places non-overlapping transients into shared VMA
    placed/aliased allocations. TWO correctness rules: (a) **aliasing
    barrier** at every handoff — acquire from UNDEFINED (content is garbage
    by definition) and synchronize against the PREVIOUS TENANT's last
    access, not the resource's own history; (b) **frames-in-flight ringing**
    — lifetimes are per-frame-slot; in-flight frames never share a
    placement.
  - **Scratch API**: passes declare per-frame scratch byte needs; the graph
    bump-allocates from one per-frame-slot scratch heap (worklists, counts,
    froxel lists, readback staging are candidates — anything reset every
    frame).
  - Gates: parity AE=0 everywhere; **VRAM before/after reported** (the
    acceptance number, index-heap-removal discipline); sync validation
    clean.
- **M4 — Async queue policy (the perf payoff):**
  - The graph's scheduler places dependency-free compute chains onto async
    compute lanes (candidates from Tracy: HiZ pyramid build, froxel binning,
    expansion scans, IBL prefilter when 07 lands) with timeline-semaphore
    edges and queue-family ownership handling (prefer SHARING_CONCURRENT
    only where profiling shows ownership transfers are the bottleneck;
    default to explicit transfers).
  - Policy degrades explicitly by capability table (1-lane hardware = same
    graph, serialized placement, zero special cases).
  - Gates: parity AE=0 run-twice; Tracy multi-queue capture showing actual
    overlap; per-pass table re-captured on all three cameras; report the
    frame-time delta honestly (async wins are often modest at low res —
    the structural win is the point).
- **M5 — Close-out**: hand-barrier deletion grep gate final, docs (a short
  "how to declare a pass" section — usages, scratch, lane hints — appended
  to this brief), 06 baseline table updated, net LOC and VRAM numbers.

## Acceptance

- All milestones parity-gated (AE=0 standard captures + run-twice); flake
  check + .#demo green; NORMAL validation clean throughout AND sync
  validation clean at M2/M3 gates; hot reload + freeze + all CVar levers
  unaffected.
- VRAM reclaimed by aliasing reported; multi-queue overlap demonstrated in a
  Tracy capture; frame-time deltas per camera reported honestly.
- NEEDS VISUAL VERIFY: flythrough after M2 and M4 (sync bugs manifest as
  flicker/corruption under motion, which settled captures can miss).

## How to declare a pass (M5 doc — the post-04e authoring contract)

> **SUPERSEDED (2026-08-07, briefs 20/21). DO NOT FOLLOW THIS SECTION.** Every API it instructs
> you to use — `Pass::usages`, `ResourceUsage`, `record_compute()`, `Access::` + stage masks,
> `async_usages`/`has_async_compute()`, `PassContext::scratch` reservation, per-slot re-appending
> in `update()` — was DELETED by brief 20. Passes are now declared once through the fluent
> `frame_graph` API over logical handles (`fg.pass("name").reads(x).writes(y).raster(cb)`); see
> brief 20's End state and brief 21. The intra-pass-scratch / uploaded-inputs / host-ring
> exemptions in item 1 were explicitly REVOKED by brief 20. Only the top paragraph's rule ("a pass
> declares WHAT it touches; the renderer derives") survives — it is the rule everything else now
> implements.

A pass declares WHAT it touches; the renderer derives ordering, barriers,
layouts, attachments, and queue placement from the declarations. Do not write
`vkCmdPipelineBarrier2` for anything that crosses your pass's boundary.

1. **Frame usages** (`Pass::usages`, `std::vector<ResourceUsage>`): one entry
   per (resource, Access, stage-mask) your record_compute()/record() touch.
   - Attachments: `{COLOR_TARGET, ColorWrite, COLOR_ATTACHMENT_OUTPUT}`,
     `{DEPTH_TARGET, DepthWrite, EARLY|LATE_FRAGMENT_TESTS}` (or `DepthRead`
     for test-only passes — a DepthRead still binds the attachment).
   - Buffers your compute writes and your draws read:
     `{buf, StorageWrite, COMPUTE}` + `{buf, IndirectRead, DRAW_INDIRECT}` +
     `{buf, StorageRead, TASK/...}`. The graph emits the minimal merged
     barrier; the old renderer-wide compute->draw barrier no longer exists.
   - Per-frame-slot resources: re-append the CURRENT slot's ids each
     update() (truncate to your static prefix first — see GeometryPass).
   - Persistent RMW state (atomics): declare `StorageWrite` at the touching
     stage; its scope is READ|WRITE so cross-frame ordering falls out.
   - What NOT to declare: intra-pass scratch consumed inside one record hook
     (keep local barriers), static uploaded inputs (transfer batch owns their
     one-time transition), host-written rings (submission-order visible).
2. **Scratch** (`PassContext::scratch`): reserve per-frame transient bytes at
   construction (`off = context.scratch.reserve(bytes)`); address at record
   time as `scratch.buffer(slot)` / `scratch.address(slot) + off`. Contents
   are garbage at frame start BY DEFINITION — write before read, every frame.
3. **Async compute** (`Pass::async_usages` + `has_async_compute()` +
   `record_async_compute()`): a chain with NO same-frame GPU dependencies.
   Write usages = what the chain produces (on the async queue); read usages =
   where the main-queue frame consumes it. The renderer derives the timeline
   edge + queue-family ownership transfer, or records inline when no lane
   exists (capability table). Levers: `r.async.enabled` / `STRING_ASYNC`.
   Do NOT open Tracy GPU zones inside record_async_compute — the renderer
   wraps the chain in the LANE's own context.
4. **Ordering**: the graph toposorts by declared usage (stable — authored
   order wins when valid). If your pass must run after another, express it
   through a shared resource, not by list position alone.

## Running log (2026-07-23 agent session)

### Gate methodology (learned from the 04d post-close saga, locked for this brief)

- Captures are pinned to PREBUILT store paths + a shader-source snapshot
  (`/tmp/04e/capture_fixed.sh`), never rebuilt from the live working tree —
  an earlier in-session attempt silently compared captures built from
  different tree states when edits landed mid-capture-job.
- Resolution set: 800x800, 1024x1024 (>=1024 pyramid base -> 11-mip HiZ, the
  sparse-mip regression class), 1280x720 (non-square), 1920x1080. NOTE: this
  display clamps window sizes above ~1920x1131 (a 1600x1600 or 1920x1080
  request both come out 1920x1131); the clamp is deterministic so A/B pairs
  still align exactly.
- Cameras: interior `9,4.5,0,3.1416,0.05`, exterior `35,30,25,-2.52,-0.51`,
  crowd = exterior + `STRING_CROWD=1`. Plus interior `STRING_HIZ=0`.
- Motion: `STRING_ORBIT=0.5` + `STRING_FIXED_DT=0.016666` numbered sequences
  (`/tmp/04e/capture_seq_fixed.sh`), compared frame-index-aligned.

### M1 — queue/command infrastructure (implemented)

- `queue_family_caps` capability table enumerated at physical-device
  selection (family flags, queue counts, present support, timestampValidBits),
  logged at startup. `device` now creates the FULL useful queue set: 1
  graphics/present/transfer queue per family + up to 4 queues of a DEDICATED
  compute family, and exposes them as named `submission_lane`s ("main",
  "async-compute-N", "transfer") — 1:1 lane->queue, existence
  capability-derived, no transparent multiplexing (locked guardrail). On this
  RADV/RDNA3 device: `main, async-compute-0..3` (RADV exposes no dedicated
  transfer family).
- `string::gpu::submission_set` (new): per-lane timeline semaphore +
  per-(lane, frame-slot) command recorders. The renderer's frame pacing
  timeline IS the main lane's timeline (the ad-hoc `frame_semaphore_`
  creation is deleted); `Frame` no longer owns a recorder — acquisition is
  `submissions_.recorder(lane, slot)`. Multithreaded recording explicitly out
  of scope (the (lane, slot) recorder is the seam).
- Tracy: one GPU context PER LANE (named after it), created with a probe
  buffer from that lane's own pool; lanes without valid timestamps get none.
  `gpu_profiler_ctx_` (what passes receive) aliases the main lane's context.
- Planner groundwork (inert): the render-graph builder implementation moved
  from the unbuilt example into the library (`src/vulkan/render_graph.cpp`)
  with a STABLE Kahn toposort (authored-order-preserving; declaration order
  directs same-resource read/write edge direction, making the graph acyclic
  by construction) + `render_graph_test.cpp` gtests.

### M1 gate results (PASS, 2026-07-23)

- nix flake check green (engine + cook tests; includes the new render_graph
  gtests); .#demo builds.
- Parity vs the committed-HEAD baseline binary, pinned stores: interior /
  exterior / crowd x {800x800, 1024x1024, 1280x720, 1920x1080} + interior
  HiZ-off — **12/13 AE=0**. The 13th (crowd 1024x1024) differed in ONE batch
  run and was NOT reproducible: 4 fresh runs (2 baseline + 2 M1) are all
  AE=0 against each other and the baseline. The anomalous image is the whole
  scene at COARSER TEXTURE MIPS (visually blurrier, mean diff ~18/255 —
  a texture-streaming timing flake under heavy machine load: three nix
  builds ran concurrently with that capture). Filed as a gate-hygiene rule:
  captures now run with the machine otherwise idle. Also worth knowing: this
  is a PRE-EXISTING streaming-timing sensitivity, not an M1 behavior change.
- Startup log shows the capability table + `submission lanes: main,
  async-compute-0..3` on RADV/RDNA3; validation shows only the benign dzn
  ICD-skip line.

### M2 — graph-driven execution + derived barriers (implemented)

Single source of truth: pass-declared `ResourceUsage` now drives scheduling
AND sync.

- `record_frame` builds a `RenderGraph` from the frame passes' declared
  usages EVERY frame and executes in its toposorted order (stable == authored
  order when that order is valid — the parity anchor).
- `ResourceStateTracker` rewritten hazard-complete: writes barrier against
  the last write AND accumulated readers (WAW/WAR, even with no layout
  change — the 04d msaa ghosting class); reads only pay when the last write
  isn't yet visible to their (stage, access); BUFFERS tracked by resource_id
  with hazards merged into ONE `VkMemoryBarrier2` per flush point.
- Access model: `ColorWrite` scope widened to WRITE|READ (LOAD_OP_LOAD,
  blending, resolve-source reads live in the same stage), `StorageWrite` to
  WRITE|READ (atomics/RMW), new `Access::IndirectRead`.
- msaa_color_ is a FIRST-CLASS tracked resource: the logical COLOR_TARGET
  expands to the physical MSAA image inside the group loop and the tracker
  derives the clear/store->load/resolve ordering.
- Geometry pass DECLARES its inter-phase buffers per frame slot (update()
  re-appends: opaque/two-sided worklists StorageWrite@COMPUTE +
  IndirectRead@DRAW_INDIRECT + StorageRead@TASK; froxel list
  StorageWrite@COMPUTE + StorageRead@FRAGMENT; visibility bitfield
  StorageWrite@TASK). Shadow worklists/scan scratch stay intra-pass (written
  and consumed inside record_compute with local barriers — allowed by the
  brief).
- HAND BARRIERS DELETED (the win metric):
  1. renderer.cpp broad compute->draw `VkMemoryBarrier2` (fixed
     COMPUTE -> INDIRECT|VERTEX|FRAGMENT) -> replaced by per-buffer derived
     scopes merged per flush point;
  2. renderer.cpp msaa_color_ cross-frame self-barrier (04d fix #3);
  3. renderer.cpp msaa_color_ store->load ghosting self-barrier (04d motion
     fix) — both now fall out of the tracker's WAW/WAR handling;
  4. geometry_pass.cpp cross-frame visibility-bitfield TASK->TASK barrier
     (04d fix #2) -> derived from the declared visbits usage.
  KEPT (documented exceptions): the hz.depth resolve-target transition in the
  renderer (intra-pass scratch whose post-resolve state is managed by the
  pass's local barriers), geometry's intra-pass compute-stage barriers
  (draw-cull->expand->fill chain, shadow cascade transitions, HiZ mip chain),
  transfer_batch upload barriers, ui_pass glyph-atlas upload transitions.
- Attachment derivation (depth read fallback + forced write scope for bound
  depth) unchanged from the 04d fixes — still usage-derived.
- nix flake check green on the M2 tree (engine + cook tests). Smoke:
  interior 800x800 AE=0 vs baseline, [mesh-cull] stats byte-identical.
- Found by the gate (worth keeping): a pass declaring TWO writes of one
  resource (stats: compute + draw stages) forged a writer-chain self-edge in
  the planner -> "cycle detected". Fixed with per-pass dedupe of
  writer/reader lists + a regression gtest. The failing store never became a
  gate reference (smoke-first rule now: every rebuilt store smokes one
  interior capture before the batch).

### M2 gate results (PASS, 2026-07-23)

- Static parity vs baseline (pinned stores): interior/exterior/crowd x
  {800x800, 1024x1024, 1280x720, 1920x1080} + interior HiZ-off —
  **13/13 AE=0**.
- Run-twice determinism (interior/exterior/crowd 800x800): AE=0 x3.
- MOTION sequence (STRING_ORBIT=0.5, STRING_FIXED_DT, every 20th frame,
  frames 20..780, phase2 nonzero mid-orbit — the interleaved phase-1/phase-2
  multi-group path): frames >=100 ALL AE=0 vs the baseline sequence. Early
  frames (20/60/80) differ — PROVEN pre-existing streaming-warmup
  nondeterminism, not an M2 regression: a baseline-vs-baseline rerun shows
  the same magnitude diff at frame 60 (AE 230k vs 237k) while M2 matches
  baseline exactly once streaming settles.
- Sync validation (VK_LAYER_VALIDATE_SYNC=1, 1024x1024 static + orbit
  sequence): exactly 4 hazard lines, all the PRE-EXISTING
  vkAcquireNextImageKHR WAR class — byte-for-byte the same 4 lines the
  BASELINE binary produces under the layer. ZERO new hazards from derived
  sync. Normal validation: 0 non-dzn lines across all 13 capture logs.

### M3 — transient aliasing + scratch (implemented)

- `String::FrameScratch` (`string-engine/include/string/vulkan/frame_scratch.hpp`
  + `src/vulkan/frame_scratch.cpp`): per-frame-slot bump arena. Passes
  `reserve(bytes)` during construction (256B-aligned offsets, identical in every
  slot); the renderer `materialize()`s ONE device-local buffer per frame slot
  after all passes are built and exposes it via `PassContext::scratch`.
  Correctness rules honoured by construction: (a) acquire-from-UNDEFINED —
  scratch contents are garbage at frame start by definition, nothing may read a
  region before writing it this frame; (b) frames-in-flight ringing — each slot
  has its OWN buffer, in-flight frames never share a placement.
- Geometry pass moved its per-frame transients into the arena: 6 worklists
  (opaque + two-sided + 4 shadow cascades) + the shared draw_lod buffer, x3
  slots — 21 dedicated VMA allocations replaced by 3 slot buffers
  (`[scratch] 2722 KiB per frame slot x 3 slots (8168 KiB total)`).
- **VRAM before/after (the honest acceptance number): ~0 bytes reclaimed.**
  Before: 21 allocations totalling ~8,166 KiB; after: 3 allocations totalling
  8,168 KiB (256B inter-region alignment vs 16B). Lifetime analysis found NO
  non-overlapping transient pairs in today's frame — every candidate spans the
  geometry pass (worklists live from draw-cull compute through the last shadow
  cascade draw), so regions pack disjoint. The payoff at this frame content is
  structural: 21 -> 3 allocations, and the arena + planner lifetimes are the
  substrate future passes (VFX/post transients, 07 IBL scratch) alias into
  for free.

### M3 gate results (PASS, 2026-07-23, STRING_ASYNC=0 — arena without async placement)

- Store pinned: `/nix/store/1kwn4a9wbq17zg8n3fpb2c3d00ha2dp6-string-demo-0.0.1`
  (verified == a fresh `.#demo` build of this exact tree; shader snapshot
  content-identical to the pin).
- Static parity vs baseline: interior/exterior/crowd x {800x800, 1024x1024,
  1280x720, 1920x1080} + interior HiZ-off — 12/13 AE=0 in the batch; crowd
  800x800 hit the KNOWN pre-existing streaming-mip flake (batch ran under
  machine load, M1 anomaly class) and an idle retake is AE=0. Net: 13/13.
- Run-twice (interior + crowd 800x800): AE=0.
- Motion sequence (STRING_ORBIT=0.5, STRING_FIXED_DT, every 20th frame): 156
  frames compared vs the baseline sequence, ALL settled frames (>=100) AE=0.
- Sync validation (1024x1024): exactly the 4 pre-existing
  vkAcquireNextImageKHR WAR lines, byte-identical to the baseline binary's
  modulo ASLR'd handles. Normal validation: 0 non-dzn lines in all capture logs.

### M4 — async queue policy (implemented)

- Candidate: froxel light binning — the only chain in today's frame with NO
  same-frame GPU dependencies (reads only the host-written light SSBO ring; its
  froxel shader fully overwrites every cell, so acquire-from-garbage across the
  family transfer is valid). Moved from `record_compute` to
  `GeometryPass::record_async_compute` + declared in `async_usages`
  (StorageWrite@COMPUTE produced on the async queue, StorageRead@FRAGMENT
  consumed by the main-queue frame).
- Renderer placement (renderer.cpp, DERIVED from async_usages — no hand-placed
  cross-queue sync): with an async lane + `r.async.enabled` (env alias
  STRING_ASYNC, default on), all chains record into one command buffer on the
  lane, submit immediately signalling the lane timeline at frame_count_;
  queue-family ownership is transferred EXPLICITLY (release barrier on the
  async queue, acquire on main — the locked default) and the main submit waits
  the lane timeline at the union of the declared read stages. Without a lane
  (or lever off) the SAME chains record inline on main with tracker-derived
  barriers — zero special cases (capability-table degrade, locked guardrail).
- Cross-frame WAR safety: begin_frame's CPU wait on the main timeline for the
  frame slot completes BEFORE the slot's async submit is recorded, so the main
  queue's last read of that slot's froxel buffer has retired GPU-side —
  happens-before via host, no extra semaphore needed.
- Tracy: the async chain is wrapped in the LANE's own GPU context (M1 per-lane
  contexts); passes must NOT open GPU zones inside record_async_compute.

### M4 gate results (PASS on correctness; overlap NOT demonstrated — 2026-07-23)

- Static parity vs baseline (pinned store, STRING_ASYNC=1): 13/13 AE=0 (crowd
  1280x720 flaked in-batch under load, idle retake AE=0 — same M1 class).
- Run-twice (interior + crowd): AE=0. Motion sequence: 159 frames vs baseline,
  ALL settled frames (>=100) AE=0 (frames 60/80 = proven pre-existing
  streaming warmup; the final frame of the batch was cut by the prior session
  limit — 158 settled frames compared, all zero).
- Sync validation, static AND motion, async ON: exactly the 4 pre-existing
  acquire-WAR lines, zero new — the derived QFOT + timeline edges are clean
  under the layer. Normal validation 0 non-dzn everywhere.
- Per-pass GPU table re-captured on all three cameras (RADV/RDNA3, 1024x1024,
  `.#demo-tracy`, settled means, async on/off):

  | zone (ms)        | int a1 | int a0 | ext a1 | ext a0 | crowd a1 | crowd a0 |
  |------------------|-------:|-------:|-------:|-------:|---------:|---------:|
  | draw-cull        | 0.015  | 0.014  | 0.013  | 0.014  |  0.227   |  0.225   |
  | expand           | 0.025  | 0.025  | 0.025  | 0.025  |  0.057   |  0.058   |
  | froxel-cull      | 0.269  | 0.241  | 0.218  | 0.214  |  0.240   |  0.208   |
  | shadow-cascades  | 0.389  | 0.390  | 0.381  | 0.390  | 11.637   | 11.487   |
  | sky              | 0.189  | 0.187  | 0.235  | 0.234  |  0.210   |  0.209   |
  | main-draw        | 4.398  | 4.341  | 1.670  | 1.641  | 11.385   | 11.331   |
  | hiz-build        | 0.104  | 0.102  | 0.106  | 0.105  |  0.103   |  0.102   |
  | phase2           | 0.326  | 0.322  | 0.161  | 0.163  |  3.209   |  3.216   |
  | composite        | 0.070  | 0.069  | 0.044  | 0.045  |  0.066   |  0.067   |
  | frame time       | 6.987  | 6.859  | 4.371  | 4.157  | 28.769   | 28.457   |

- **Honest frame-time delta: async ON is +0.13 / +0.21 / +0.31 ms SLOWER**
  (interior / exterior / crowd) — the extra submit + timeline + QFOT costs buy
  nothing today because **measured overlap is ZERO** (exact interval check
  over every settled froxel-cull instance vs all main-queue GPU zones, all
  three cameras). Root cause is NOT the graph: the demo acquires the swapchain
  image in begin_frame BEFORE recording, so with ~3 swapchain images a
  GPU-bound frame blocks acquire until the previous frame completes — the CPU
  never runs ahead, and the async submit lands in the inter-frame GPU idle gap
  (Tracy timeline: composite(N-1) ends, froxel-cull(N) runs on the async
  queue, THEN geometry(N) starts ~0.5 ms later). PROPOSED FOLLOW-UP (pacing,
  out of 04e's locked scope): acquire after record (late-latch) or otherwise
  restore CPU run-ahead; overlap should then appear with zero graph changes.
  The structural win (derived cross-queue sync, capability degrade, per-lane
  Tracy) is shipped and gated; STRING_ASYNC=0 remains a one-flip lever if the
  ~1% cost matters before the pacing follow-up lands.

### M5 — close-out (2026-07-23)

- Hand-barrier grep gate (`vkCmdPipelineBarrier` across string-engine/src +
  sandbox): every remaining site is a documented class —
  - `src/vulkan/resource_state.cpp` (1): THE tracker's derived-barrier emitter.
  - `src/vulkan/renderer.cpp` (2): the M4 QFOT release/acquire halves —
    GENERATED per-resource from declared async_usages, not hand-placed.
  - `src/vulkan/transfer_batch.cpp` (1): upload batch (documented KEPT).
  - `sandbox/passes/geometry_pass.cpp` (7): all INTRA-pass (stats-reset before
    draw-cull atomics, draw-cull->expand->fill chain x3, shadow cascade
    transitions, HiZ mip chain in record_between x2) — the brief explicitly
    allows local barriers for scratch consumed inside one record hook.
  - ui_pass glyph-atlas upload transitions (documented KEPT).
  HAND BARRIERS DELETED list unchanged from M2 (4 renderer/geometry frame-level
  barriers); M3/M4 added no hand sync — that is the point.
- Docs: the "How to declare a pass" authoring contract is the section above
  (usages, scratch, async, ordering). 06 per-pass baseline table updated
  (post-04e hardware numbers appended to 06-debug-tooling.md).
- Net code LOC (string-engine + sandbox, vs pre-04e HEAD): +1396 / -448.
  VRAM: ~0 net (see M3 — 21 -> 3 allocations, no aliasable lifetime pairs in
  today's frame).
- nix flake check green on the final tree; .#demo == the pinned gate store.
- Not re-verified this session (unchanged code paths, M1/M2 discipline):
  shader hot reload + freeze levers. Flag for the visual verify pass.

### NEEDS VISUAL VERIFY (user)

- Flythrough interior + exterior + crowd (and once with STRING_ASYNC=0):
  sync/aliasing bugs manifest as FLICKER or CORRUPTION under motion — settled
  captures cannot catch them. Failure looks like: sparkling/garbage patches in
  lit areas (froxel list races), smearing/ghosting on camera cuts (aliasing),
  or one-frame black/garbage flashes (QFOT). All twelve gate cameras were
  byte-identical, so anything visible under motion is new information.

### Follow-up — late-latch swapchain acquire (2026-07-23, post-close-out)

The M4 pacing follow-up: move `acquire_next_frame()` from `begin_frame()` (before
recording) to `record_frame()`, at the first point the frame actually needs the
image — the composite group. Graph/derived-sync layers untouched, as designed.

- **What moved:** `Renderer::begin_frame` no longer acquires; `record_frame`
  latches the image via a one-shot `acquire_swapchain` lambda invoked when the
  group loop reaches the SWAPCHAIN_TARGET group (plus a defensive call before
  the final present transition). Everything recorded before that point — the
  async submit, compute prepass, all offscreen groups — references no swapchain
  state, so no command-buffer split was needed: the frame stays ONE main CB,
  with the acquire happening mid-record between the offscreen groups and the
  composite group. Smallest correct change.
- **First-touch invariant:** the main submit in `end_frame` remains the first
  and only submission touching the image; it still waits the acquire semaphore
  at COLOR_ATTACHMENT_OUTPUT. The async submit carries no swapchain reference.
- **OUT_OF_DATE path:** surfaces inside the presenter's existing
  recreate-and-retry loop, now mid-record. Safe because nothing recorded yet
  references the swapchain; the fresh image/view is latched before any
  swapchain command. No frame drop, no fence/semaphore/timeline leak (the
  acquire semaphore is NOT signalled on OUT_OF_DATE, so retrying with it is
  legal). SUBOPTIMAL fix folded in: it DOES acquire + signal, so the presenter
  now accepts it as a successful acquire (present() already triggers the
  recreate) instead of retrying with an already-signalled semaphore.
- **Pacing:** the CPU throttle is now solely the per-slot timeline wait at the
  top of begin_frame — confirmed in the trace below (acquire dropped from
  ~20 ms to ~0.04 ms; begin_frame's timeline wait became the ~21 ms pacer).

Verification (deliberately LIGHT — user does the real run):
- nix build .#demo green.
- Headless smoke, interior 800x800 (STRING_CAM="9,4.5,0,3.1416,0.05",
  frame 300): non-black, **byte-identical (AE=0)** vs a pre-change capture
  taken from the same tree. Only the known-benign dzn ICD validation line in
  both logs.
- Overlap check (crowd, .#demo-tracy, interval method): **overlap still NOT
  demonstrated headlessly** — 2/325 froxel-cull instances intersect a
  main-queue zone; froxel-cull still executes in the ~0.9 ms inter-frame gap.
  HOWEVER the CPU-side picture changed as intended: "Acquire Next Frame" is
  now ~0.04 ms (headless surface negotiates MAILBOX) and submissions run
  ahead of GPU completion. Best current theory for the residual serialization,
  undecidable headlessly: (a) the acquire-semaphore wait at
  COLOR_ATTACHMENT_OUTPUT gates ALL color output of the single main CB (incl.
  the first MSAA group) on image availability — with 3 images GPU-bound that
  re-serializes the main queue per display period; and/or (b) this
  machine/driver does not execute the compute queue concurrently with a
  saturated gfx ring (would also explain the original M4 zero-overlap).
  Cross-lane Tracy timestamp calibration is a further caveat. Judge on the
  live Tracy GUI run; if (a), the next lever is splitting the composite into
  its own submit so only IT waits the acquire semaphore.

NEEDS USER VERIFY (live, cannot be tested headlessly):
- **Window resize** (the riskiest path: OUT_OF_DATE now fires mid-record),
  including rapid/continuous resizing.
- Alt-tab / focus loss and fullscreen toggle (SUBOPTIMAL + OUT_OF_DATE mix).
- General flythrough (interior/exterior/crowd) for flicker/garbage frames.
- Tracy GUI: does froxel-cull now overlap frame N-1's main-queue work?
