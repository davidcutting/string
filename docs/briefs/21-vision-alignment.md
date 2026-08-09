# Brief 21 — Vision alignment

Status: **DRAFT — decisions adjudicated with the user 2026-08-07; sequencing approved pending user
review of this document.** Follows the full 4-agent audit of brief 20's landed state (2026-08-07).

## Why this brief exists

Brief 20 landed the core design: one declaration channel, two-state graph, no pass base class,
descriptor_table restored, framework-opens. The audit confirmed that — and also found that the drift
did not disappear; it moved *around* the graph (FrameScratch, the temporal misfiling, transfer_batch,
compile-time clears), that several of brief 20's completion claims are false, and that the doc record
contains contradictions and fabricated measurements that future agents will re-inherit.

**Rule for this brief (user, 2026-08-07): alignment before bug fixes.** Several open bugs sit ON
unresolved architecture; fixing them first would harden the drift in place. Each bug is fixed at the
step that aligns the architecture it sits on, and is listed there.

## Decisions (user, 2026-08-07) — supersedes conflicting statements in briefs 04e/11/16/20

These four were adjudicated explicitly after the audit surfaced provenance conflicts. They are the
standing record; earlier contradicting text is superseded and gets marked so (see Doc repairs).

**D1 — Framework-opens: RATIFIED.** The executor derives load/store/resolve/MRT/multiview and calls
`vkCmdBeginRendering` itself. This is a deliberate deviation from Daxa's task-opens, now genuinely
user-confirmed (brief 20 claimed this before it was true; brief 16 correctly labeled it agent-shaped
at the time. As of today it is decided).

**D2 — Authored once, compiled once — softly stated.** The graph is authored and compiled once at
startup. Toggles are in-graph conditionals; resize is a backing swap; pipeline/asset hot-reload swap
objects *inside* passes and never touch the graph. There is no recompile trigger because nothing
needs one — **not** because a second compile is sinful. If runtime graph *editing* ever becomes a
feature, recompile becomes legal at that seam; until then it does not exist. The "a second compile is
a gate failure" absolutism in brief 20 is retired; the gate becomes "toggling and resizing do not
recompile." Supersedes brief 11's locked "toggle/resize/hot-reload force a recompile" (11-L38).

**D3 — Time lives outside the graph (the canonical shape).** Confirmed original intention: a
persistent resource is created and managed OUTSIDE the graph; the graph tracks usage within the
frame. Consequences:
- **Cross-frame history = a persistent pair + an owner swap** (`std::swap` + `set_images`), the
  Frostbite/Unreal/Daxa pattern. No `.reads_history()` verb; no temporal concept in the graph.
- **In-place read-modify-write = `.read_writes()`**, restored from brief 11's locked decision. One
  combined access, GENERAL layout, no ping-pong implied. The `.mip(0)` slice trick dies.
- **`per_frame` (deviation A1) is DELETED with no replacement.** A frames-in-flight pacing ring is a
  multi-slot persistent (`physical = {a, b}`, resolved by frame slot — already supported). A
  transient is single-backed by definition.
- **History validity**: after startup and after resize, `prev` holds garbage for one frame. Per
  consumer: clear at creation (neutral + one-time clear) or skip reprojection one frame. GTAO takes
  the one-frame fallback.

**D4 — The graph owns transients and their aliasing; FrameScratch is deleted.** Each meshlet
worklist becomes its own `fg.buffer()` transient; `materialize()` gets real interval packing
(lifetimes from toposort order; 04e's two rules: acquire-from-UNDEFINED at each handoff synchronised
against the previous tenant, per-slot lifetimes so in-flight frames never share a placement). This
also recovers the cull/cascade parallelism lost to the single-arena serialisation.

**The category test that D3+D4 restore** (the audit found these exactly swapped in two places):
contents outlive the frame → persistent, owner-managed (`hiz.depth` was misfiled as a `per_frame`
transient); contents are frame-scoped scratch → transient, graph-managed (worklists were misfiled as
`use_persistent` pointed at FrameScratch).

---

## LANDED SO FAR (2026-08-07, uncommitted working tree)

**Part 1 — DONE.** All five doc repairs are in (brief 20 STATE BANNER, 04e SUPERSEDED banner, 16
status + three retractions, 11 supersession marks, README statuses **and** its broken numbering,
which ran 1-11 then restarted at 7 and ended with `14e.`/`14f.`).

**Step 1 — DONE.** `hiz.depth` is an app-owned persistent pair resolved by frame slot; the five
pacing rings are multi-slot persistents; `per_frame` is gone from both transient infos;
`.read_writes()` is back and probe-GI hysteresis uses it; the `.mip(0)` trick is gone; the app's
`record_of().physical` reach-in is gone and `record_of`/`image_record`/`buffer_record` are **private
again**. `set_images` hardening landed as a *deferred* re-pointing: a swap records what it retired,
`execute()` applies it at the frame boundary (the only safe moment) and marks the graph executing so
a mid-frame swap is refused with an error rather than corrupting. A pure PERMUTATION of the same ids
— the per-frame history rotation, which happens twice a frame — is diffed out and costs nothing.
- `post.histogram.bins` deliberately stays a TRANSIENT, against this brief's "five pacing rings"
  list: it is cleared, accumulated and reduced inside one frame. Its contents do not outlive the
  frame, so by D3's own category test it is not a persistent.

**Step 2 — DONE (code), NOT UNDER TEST.** Resize is wired end to end, and the new
`STRING_RESIZE_AT=frame:WxH` lever makes it **reachable** from a headless run, because an offscreen
SDL window never emits a resize event and nothing headless could otherwise reach the path at all.

> **CORRECTED 2026-08-08.** This step originally claimed resize was "for the first time,
> **testable**", which read as *tested* and was cited that way. It is not. `string-core/test/`
> contains **zero occurrences of "resize"** — no automated test exercises the path. What exists is a
> lever that lets a human drive one manually, plus the one-off runs transcribed in step 7. A
> regression in resize would be caught by nobody today.
`hiz.pyramid` is viewport-derived: `viewport_fit::half_pow2` + `mip_levels = all_mips` declare the
RELATIONSHIP (replacing the `viewport_scaled` bool with a `viewport_fit` enum), and the reduction
chain is authored once at `kMaxHizMips` with each level conditioned on `m < hiz_mip_count(viewport)`.
Out-of-range slices resolve to 0 so binding skips them. The startup-bucket clamp workaround is
deleted. Per-frame clear/load ownership was already derived at execute over survivors.

Three defects found and fixed while gating step 2 — none of them in this brief's list, all found by
running sync validation at a non-square extent, which nothing had done:
1. **`.depth()` declared only `EARLY_FRAGMENT_TESTS`.** A depth attachment is written at EARLY *and*
   LATE; naming one left every later reader's derived barrier missing the late write. Sync validation
   reported it as a WAW against `vkCmdEndRendering` on every frame the HiZ chain sampled the resolved
   depth — i.e. always.
2. **A cleared attachment was barriered as a read.** A group that only TESTS depth can still be the
   first to bind it, and then the framework writes it through `loadOp CLEAR` against a barrier that
   granted READ, in the read-only layout. Framework-opens means the framework owns load/store/resolve,
   so it owns their synchronisation and their layout: the clear now promotes both.
3. **The multisample resolve's scope.** A depth resolve lands in the depth-attachment layout but the
   write is attributed to `COLOR_ATTACHMENT_WRITE` at `COLOR_ATTACHMENT_OUTPUT`, so a barrier naming
   only the depth stages neither permits the resolve nor covers it for the next reader. Added
   `resource_state_tracker::transition_scope` — the framework describing its own write, which is what
   deleting `Access::DepthResolve` from the DECLARED surface always presumed. Not new declared
   vocabulary: no pass can name it.

Result: **0 validation errors and 0 sync hazards** under motion + resize at 1600x900 (was 10 hazards
at entry). 77/77 gtests (one expectation updated to the corrected depth stages).

**IBL products re-filed as persistents (D3, found by gating step 2).** `ibl.env_capture`,
`ibl.env_prefiltered`, `ibl.dfg` and `ibl.sh` were transients, so a resize destroyed their backing —
and the bake that fills them is amortized, so it did not re-run. Every frame after a resize shaded
against 1x1 neutral env + DFG: the washed-out, over-bright look the user reported as "certain screen
sizes result in strange artifacts". Same category and same fix as the probe-GI atlases. Degrade
substitutions across a resize went from continuous to **2 at startup**.

**Step 3 — DONE (except aliasing).** `FrameScratch`, `ScratchLifetime`, `engine_context::scratch`,
`renderer::scratch_`/`materialize_scratch`, `wire_scratch_arena`, `GeometryScene::scratch_`, the
shadow list binding and the frame_scratch test are all DELETED. The six work lists + the shared
draw_lod are individual `fg.buffer()` transients (`WorklistSet`), single-backed per D3 — so the
per-slot arena's 3x memory goes with it, and each cull/expand/draw chain names its own buffer, which
de-serialises the cascade chains from the camera's. Observable effect: `phase2` went from **0 to ~75
draws/frame** — two-phase occlusion is picking up newly disoccluded meshlets again, which it could
not do while everything serialised through one arena handle.
- **Transient accounting fixed**: `transient_bytes_` counted BUFFERS only, so the "KiB transients"
  line was fiction. Real figure, now logged at compile: **22 transients, 125.6 MiB**, no aliasing.
- **Interval packing NOT done.** It is the one item in this brief that ADDS code (~200 lines of VMA
  sub-allocation + acquire-from-UNDEFINED handoff barriers) rather than removing it, and it is a
  memory optimisation, not an alignment. With the real number now measured, that is a decision worth
  making deliberately rather than by default. Everything else in step 3 is done.

**Step 4 — DONE, with two of its three degrade deletions REFUSED on evidence.**
- `scene.upload` now writes `SceneData.froxels`. The field was NEVER written — the comment claimed it
  was "patched in record()" and nothing patched it — so it held a null device address that
  `lighting.slang` dereferences the moment a scene has local lights (`froxel_planes.w > 0`). It
  survived only because the demo scenes light with the sun alone. That is the crash-class bug.
- `meshlet.reset` split into `meshlet.reset.lists` / `.stats` / `.visbits`, three declared passes
  with three different consumers. **Both remaining hand-rolled barriers in the render passes are
  gone** — a grep for `vkCmdPipelineBarrier2` outside the tracker emitter, `vulkan_utils` and
  `transfer_batch` now returns **zero**. `record_cull` itself is deleted; the CPU state it was
  setting (`ensure_hiz`, `two_phase_active_`) moved to `tick()`, where a toggle evaluated at execute
  reads THIS frame's answer instead of last frame's.
- The composite LUT declares a CLAMP sampler on the image; the hand-made `lut_sampler_` that was
  created and destroyed but never bound is deleted.
- **Degrades: one deleted, two kept.** The `cascade_count` ternary is gone — the cascades are
  transients declaring `neutral = 1.0`, so shadow-off already resolves to the fallback. The other two
  are NOT deletable as this brief assumed, and the assumption is now falsified:
  - `gtao_slot = ~0u` is GTAO's ONLY sound degrade. `gtao.ao` holds an ENCODED bent normal; every
    constant decodes to a degenerate vector (0.5s → the zero vector), which is what collapsed the sun
    and specular terms into giant black regions earlier today. A neutral texture cannot stand in.
  - `probe_gi = 0` guards a null component, and `gi.irradiance` is a PERSISTENT since step 1 —
    persistents deliberately have no fallback, so there is nothing for it to degrade to.

**Step 6 — DONE except A5, which is REFUSED.** `resource_type`, `memory_type`, `COLOR_TARGET`,
`DEPTH_TARGET`, `is_buffer_access`, `size_fn` and `lut_sampler_` are deleted; `render_group::color` is
derived from `colors` rather than stored beside it.
- **A2**: `bytes_for` (a `std::function`) became two declared fields — `tile_pixels` +
  `bytes_per_tile`. That IS the relationship the one caller had (a froxel column per screen tile), and
  now a reader of the declaration can see it without running anything.
- **C1**: `execute_info` named the swapchain three times (a `resource_id` sentinel, a `VkImage`, a
  `VkImageView`). One `acquired_image { image, view }` now; the graph already knows WHICH resource it
  is, from the declaration.
- **getenv caching**: the four `STRING_TRACE_*` lookups sat in per-frame (and, for `STRING_TRACE_IMG`,
  per-subresource) loops. Resolved once.
- **Both compiler warnings fixed** by deleting what caused them: `slot_for`'s `pass_index` parameter,
  which two of the three overloads ignored (the access, or the fact that a buffer has one slot, is the
  disambiguator — not the pass).
- **snake_case sweep**: 99 files, `namespace String` → `namespace string`, zero `String::` left. One
  real collision, caught by the compiler: a `std::hash<string::Vertex>` specialization inside
  `namespace std`, where `string::` resolves to `std::string`. Qualified to `::string::Vertex`.
- **A5 (unify `subresource` / `image_view`) is REFUSED, not skipped.** They live in different layers
  and mean different things: `image_view` is a DECLARATION with `all` sentinels, `subresource` is the
  tracker's concrete grid coordinate. Unifying them makes `resource_state_tracker` — the layer the
  graph is built ON — depend on the graph's declaration type. That is a layering inversion bought for
  nothing; the duplication is two small structs, and it is the honest kind.

**Step 7 — a one-time TRANSCRIPT, not a battery, and its green is known to be unreliable.**

> **CORRECTED 2026-08-08. Read this before citing the table below.**
>
> 1. **It is not a gate and cannot be re-run.** "Battery" implied a durable artifact; there is none.
>    These were manual invocations, transcribed here in prose. `tools/gate.sh` is a capture/AE-compare
>    script and runs **no** resize, **no** non-square, and **no** sync validation. Nothing in
>    `nix flake check` covers this table. Re-running it means hand-reconstructing the commands.
> 2. **It reported green over a total synchronisation failure.** Every "0 hazards" row below was
>    recorded while `resource_state_tracker::transition_scope` was recording a write's own access mask
>    as *visible*, so the read path's "already visible, skip the barrier" shortcut fired on **every
>    same-layout storage read-after-write in the engine** — `storage_image_write`'s mask is a superset
>    of `storage_image_read`'s. Found 2026-08-08 via the IBL capture mip chain needing two bakes to
>    converge; fixed in `string-core/src/vulkan/resource_state.cpp`. **Sync validation did not flag
>    it.** So "0 hazards" here establishes only that the validation layer saw nothing, which is a much
>    weaker statement than this step made.
>
> What the table below IS: an honest record of what those specific runs printed on 2026-08-07. It is
> evidence, not proof, and it is not a regression net.

Every run below is sync-validation-on, one at a time:

| gate | result |
|---|---|
| exterior (800x800) | 0 validation, 0 hazards |
| non-square (1280x720) | 0 |
| fixed-dt orbit + resize to 1600x900 @ frame 120 | 0 |
| interior | 0 |
| shadow off / GTAO off / HiZ off / transparency off | 0 each |
| depth-cascade capture (`STRING_CAPTURE_SOURCE=shadow0`) | 0 |
| gtests | 77/77 |
| single-compile assertion | one `graph_.compile()` call site; one `[graph] compiled` line per run |
| degrade substitutions, steady state | 0 |
| hand-barrier grep | 7 (`transfer_batch`) + 2 (`ibl_component` verification readback); 0 in render passes |
| transient VRAM | 22 resources, 125.6 MiB, no aliasing |
| complexity gate | **FAIL** — see below |

**The per-toggle gate caught a real bug, which is what it is for.** With `r.pass.shadow=0` the scene
went DARKER, not unshadowed: `shadow.cascadeN` declared `neutral = 1.0`, and in REVERSE-Z 1.0 is the
NEAR plane — an occluder in front of everything. Degrading to it shadowed the whole scene. The correct
neutral is 0.0 (far = no occluder). The deleted `cascade_count = 0` ternary had been hiding this by
making the shader skip the lookup entirely, so the declared neutral had never once been exercised.
Fixed and re-gated: shadow-off now reads lit.

Still open: **interval packing** (step 3's one additive item), and the LIVE flythrough + resize, which
is the user's — nothing headless substitutes for it.

**Step 5 — DONE.** `TransferBatch` no longer owns a command-buffer ring, a timeline semaphore, an
async submit or a staging budget — a whole second submission path beside the frame, and the
`renderer::flush()` that pushed it, are deleted (360 → 283 lines). An upload STAGES its bytes and
QUEUES the copy; the declared `uploads.stream` pass records the queue into the frame's command
buffer. Completion is the frame's completion instead of a private timeline.
- **The WAR defect is fixed, and it was worse than described.** The old pre-copy barrier sourced from
  `UNDEFINED` at `TOP_OF_PIPE` — a scope that names no prior work at all, so it neither waited for
  in-flight sampling of the texture being overwritten nor preserved the mips it was not touching. Now
  one conservative `ALL_COMMANDS`/`SHADER_READ` barrier heads each batch, and a per-image bitmask
  decides `UNDEFINED` (first write of these levels) vs `SHADER_READ_ONLY` (already live). Bindless
  reads are undeclarable by construction — any pass may index any slot — so that one barrier is the
  honest statement of an edge nothing can derive, not a barrier the graph failed to produce.
- The post-copy destination scope was `FRAGMENT_SHADER` only; the meshlet TASK shaders sample
  bindless textures too. Now `ALL_COMMANDS`.
- **The debug capture is a declared pass** (`debug.capture`, `.reads(source, transfer_read)`), so its
  transitions DERIVE. The two hand-written ones it replaced hardcoded `SHADER_READ_ONLY` as the
  source's layout — a guess that is wrong for a depth cascade — and then corrected the tracker
  afterwards through `note_external_layout`, which is now **deleted from `compiled_frame`'s API**.
  No guess, no back channel, no second submit, no device drain before the copy.
- Surfaced by that change: **`shadow.cascadeN` never declared `TRANSFER_SRC`**, so
  `STRING_CAPTURE_SOURCE=shadowN` could never have produced a valid dump. Fixed; it now runs clean
  and writes a real depth image.
- The composite LUT bake uploaded through its own command buffer + `immediate_submit` — a device-wide
  stall reachable from a CVar edit. It goes through the transfer batch now.

Hand-written image transitions outside the tracker are down to **9**: 7 in `transfer_batch` (its own
layout management for resources the graph does not own — streamed textures are not graph resources,
which is the honest limit here, not an omission) and 2 in `ibl_component`'s numeric-verification
readback. Zero in any per-frame render pass.

**Step 7 — the headless runs printed green** (motion + resize + non-square + sync validation, 0
errors; 77/77 gtests). The live flythrough is the user's. **See the correction at step 7 above: these
were one-off manual runs, not a re-runnable gate, and their "0 hazards" was recorded over an
engine-wide read-after-write barrier failure that sync validation did not detect.**

## Part 1 — Doc record repair (first, so no agent re-inherits the drift)

No code. Each item is a surgical edit, not a rewrite.

1. **Brief 20**: add a STATE banner at top — committed as `12c1ebc`; latest true state = "IT
   RENDERS, 0 validation errors"; the 35-error `ibl.env_capture` arc, "REMAINING, in order", BUILD
   STATE, and D4 are SUPERSEDED (mark each in place). Correct the false claims in the landed-state
   section (hand barriers NOT zero — two in `record_cull`; stage masks 8 not 2; three degrades still
   present; 7/9 samplers; "nothing committed" stale). Mark the fabricated inventory cites
   (`gtao.cpp:212`, `probe_gi_component.cpp:689`, "41 sites" — real: 18) as UNVERIFIED/WRONG with
   the corrected numbers. Fix the framework-opens provenance line per D1. Note that the reference
   doc has THREE states and its `Task` IS a base class — the delete-the-base-class decision stands
   on its own merits (and D3's owner-object pattern), not on a Daxa citation.
2. **Brief 04e**: SUPERSEDED banner over the authoring contract (L117-153) — it instructs future
   agents to use `Pass::usages`, which no longer exists. Highest re-inheritance risk in the set.
3. **Brief 16**: Status → SUPERSEDED (by 20 + this brief). Retract the VUID-09600 "structurally
   impossible" guarantee (disproven by 20's G11/G15). Add a note that the `Lifetime` table inverted
   the user's ownership decision (registry-owned "Persistent" vs the original app-hands-in sketch,
   which 16's own provenance table records as traceable).
4. **Brief 11**: mark superseded-by-20/21: the recompile clause (D2), `.queue()` (became `.async()`),
   engine-owns-graph (app owns it). Mark `.read_writes()` RESTORED by D3. `.run_if()` (amortized
   IBL) is subsumed by `.toggle()` with a predicate — note it.
5. **README**: fix stale brief statuses (11-14 exist; numbering; brief 20/21 entries).

## Part 2 — Code alignment, in dependency order

Each step is independently gateable. Parity gates apply to the all-enabled config (AE=0 exterior +
non-square + fixed-dt orbit, headless, ONE capture at a time); behaviour-changing steps gate against
the degrade policy instead. Validation clean + gtest green every step. `tools/complexity.sh --gate`
delta reported every step.

### Step 1 — Temporal re-filing (D3)

- `hiz.depth`: app-owned persistent pair; per-frame `std::swap` + `set_images`; delete the app's
  `record_of().physical` reach-in (and re-privatise `image_record`/`buffer_record` — `record_of` was
  made public for exactly that one call site).
- Restore `.read_writes()`; convert probe GI hysteresis to it; delete the `.mip(0)` trick.
- Delete `per_frame` from both transient infos; the five pacing rings (scene.data, lights, stats,
  histogram.bins, transparency.list) become multi-slot persistents owned by the app/scene state.
- `set_images` hardening: assert not-mid-execute; re-point descriptor bindings for the handle's
  declared slots when backing changes (closes the "graph uses a freed id" design hole from 20, and
  is the mechanism G8 needs later).
- **Bugs fixed here**: app reach-in (audit finding 7); A1 (finding 6).

### Step 2 — Compiled-once made real (D2)

- Wire resize end to end: window event → renderer → app half (persistent owners reallocate +
  `set_images`) + `compiled_frame::resize` (graph half). Fix `resize()` itself: destroy/recreate
  consistently (no leak of non-viewport transients), re-run `bind_declared_slots`, invalidate the
  `slices_` cache, re-seed persistent layouts.
- `hiz.pyramid` becomes viewport-derived. Its mip count changes with extent, so the HiZ chain is
  authored ONCE at max mip depth with per-mip in-graph conditionals (`mip m enabled iff m <
  live_mip_count`) — the compiled-once answer to a variable-length chain.
- Per-frame clear/load ownership: derive `clears_*`/`last_*` at EXECUTE over surviving passes (the
  compile-time facts become the static skeleton; survivorship reassigns first/last writer per
  frame). A skipped pass leaves no asserted tracker state (the principled fix 20 wrote down and
  never implemented).
- **Bugs fixed here**: resize unwired/broken (finding 3); fixed-extent pyramid (finding 2); sky-off
  garbage background / toggle gate (finding 4); `record_hiz_mip` slot-by-accident (finding 6 minor —
  the conditional chain declares what it touches).

### Step 3 — Transient truth (D4)

- Six worklists → individual transient buffers; delete FrameScratch, `ScratchLifetime`,
  `engine_context::scratch`, `renderer::materialize_scratch`, `wire_scratch_arena`, and the
  frame_scratch test.
- Real interval packing in `materialize()`. Count transient IMAGES in `transient_bytes_` (today only
  buffers are counted — the reported KiB is fiction). Report VRAM before/after (the deferred 96 MB
  claim finally gets measured).
- **Gate note**: this changes barrier topology (cull/cascade chains de-serialise). Sync validation
  under motion is the load-bearing gate, not just AE.

### Step 4 — Declaration honesty in passes

- Split `meshlet.reset` into declared passes (arena zero / stats reset / visbits fill are three
  producer-consumer stages); delete the last two in-frame hand barriers.
- `scene.upload` honesty: it declares what it reads, and **writes `SceneData.froxels`** — the
  currently-null device address (crash-class bug, finding 1) is fixed by making the pass do what its
  declaration says.
- Delete the three bespoke degrades (`cascade_count` ternary, `gtao_slot = 0xFFFFFFFF`,
  `probe_gi = 0`) — with fallback binding real, shadow-off reads 1.0, GTAO-off reads white, GI-off
  reads sky-SH, per the locked degrade policy. Gate = per-toggle deltas vs policy.
- Composite LUT: create the LUT with a declared CLAMP sampler; delete dead `lut_sampler_`
  (finding 5). Delete the other stray `vkCreateSampler` once G8 is decided (below).
- Fix the dead stats readback (`if (mapped)` stub) — observability is a prerequisite for the perf
  gates, not a feature.

### Step 5 — Uploads into the graph (the least-exercised revocation — deliberately last)

- Per-frame streaming uploads become declared transfer passes (`transfer_batch` flush leaves the
  renderer; its 7 hand barriers derive; the `UNDEFINED`+`TOP_OF_PIPE` WAR defect dies with them).
- Debug capture moves out of the renderer (app-owned, per E1's own alternative), taking its two hand
  transitions and the tracker back-channel pressure with it. Renderer returns to
  acquire/pacing/submit/present in fact, not just in intent.

### Step 6 — Surface pruning (guilty until justified)

- `execute_info` swapchain named once, late-latched (C1). `size_fn bytes_for` → two declared fields
  (A2; one caller). Unify `subresource`/`image_view` (A5). `render_group`: delete the singular
  `color` beside the `colors` vector. Delete dead vocabulary (`COLOR_TARGET`/`DEPTH_TARGET`,
  `resource_type`, `is_buffer_access`, zero-caller members on `compiled_frame` — introspection
  rebuilds them WITH brief 11 M4, not before). Cache the `getenv` trace lookups out of per-frame
  loops. Fix the two compiler warnings.
- snake_case sweep (`String::` → `string::`), LAST, as always planned.

### Step 7 — Full verification battery (brief 20's, unmodified)

AE=0 all-enabled across exterior / non-square / fixed-dt orbit / interior (+ lookdev once the scene
is re-wired — currently unreachable, which blocks its gate); per-toggle deltas vs policy; resize +
motion sync validation; no-recompile-on-toggle/resize assertion; VRAM report; hand-barrier grep gate
(expected survivors: tracker emitter, construction uploads, `run_verification`); complexity gate;
**user LIVE flythrough + resize** — sync and aliasing bugs show under motion, not in stills.

## Open items needing a decision (not blocking Steps 1-4)

- **G8 — streamed-texture residency rebinding. RESOLVED IN CODE 2026-08-07 (pulled forward: it was
  the whole-scene texture corruption, not a latent design gap).** The proposal landed as written —
  `min_lod` is a field on `gpu::sampler_info`, `resource_allocator::set_sampler` swaps an image's
  sampler, and `descriptor_table::bind` publishes it. Samplers are now CACHED per configuration and
  owned by the allocator (not per image), which is what makes a mid-flight swap safe and removes the
  per-image sampler lifetime the sub-resource-view rule kept working around.

  Two defects were behind it, both brief-20 regressions:
  1. `TextureStreamer` had no residency verb at all (three call sites were `bind()` no-ops carrying a
     "BRIEF 20 REGRESSION" comment), so every streamed texture sampled from mip 0 — memory no upload
     had ever written. Flat/garbage albedo and roughness; a near-camera wall read as a sky mirror.
  2. **`descriptor_allocator::allocate()` minted a FRESH slot on every call.** A slot index is data
     the GPU already holds (material tables, SceneData), so a second `bind()` moved the resource to a
     slot nothing referenced and stranded the referenced one holding the first bind's descriptor —
     and leaked a slot each time. `bind()` could not be the rebinding verb the brief says it is until
     `allocate()` became idempotent. Worth auditing every other repeat-`bind()` path against this
     (the graph's `bind_declared_slots`, and Step 2's re-bind after resize, both re-bind).

  Still open here: no placeholder substitution (a texture is pinned to its coarsest mip until the
  coarse tail lands, rather than pointing at the white image), and the upload path itself — the
  `UNDEFINED` + `TOP_OF_PIPE` subrange WAR — is untouched and still belongs to Step 5. Sub-buffer
  ranges stay out (below).
- **Sub-buffer ranges** stay out (D4 chose separate buffers). Revisit only if a future resource is
  genuinely one buffer with disjoint declared regions.
- **Scene switching** stays deliberately unwired until after this brief; its registry request path
  is currently live-but-drained-by-nobody and should either be disconnected or left failing loudly.

## Deletions (the win metric)

`FrameScratch` + `ScratchLifetime` + scratch seams; `per_frame`; the app's `record_of().physical`
reach-in + public records; the `.mip(0)` trick; the three bespoke degrades; the two `record_cull`
hand barriers; 7 `transfer_batch` hand barriers; the capture subsystem out of the renderer; C1's
swapchain triplication; A2's callback; A5's second range type; `render_group::color`; dead
vocabulary and zero-caller surface; ~267 `String::` sites.

## Risks

- Step 2's per-frame clear derivation and Step 3's de-serialisation both change sync topology —
  motion + sync-validation gates carry those steps, stills do not.
- Step 5 (uploads) is the least-exercised path in the codebase; a wrong derived edge = one-frame
  garbage textures. It is last on purpose.
- Every step leaves the tree in the user's working tree uncommitted until they commit; ask for a WIP
  checkpoint at each green gate (the brief-20 `git checkout` loss must not repeat).
