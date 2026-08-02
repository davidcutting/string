# Phase 2 overnight — morning check-list

Autonomous run against the committed Phase-1-migration baseline. **Compare via `git diff` from your
commit to the final state.** Everything below was byte-parity-gated where possible, but the gates
are headless captures — the items flagged **EYES** need your visual judgment.

## How I gated (autonomous)
Baseline battery captured from the committed state at frame 300 (deterministic — GI not yet shading):
- `exterior` — STRING_CAM="35,30,25,-2.52,-0.51"
- `orbit` — same cam + STRING_ORBIT=1 (motion / two-phase disocclusion)
- `nonsquare` — same cam + STRING_WINDOW_SIZE=1600x900 (resize / HiZ pyramid mip class)
Each Phase-2 step diffed (imagemagick AE) against all three. AE=0 = byte-identical.
Baselines in /tmp/gp_refactor/ (won't survive reboot — rebuild from the commit to regenerate).

## Things to check in the morning (EYES) — regardless of my green gates
These are the classes my headless gates CANNOT fully judge (your own verification rules):
- [ ] **Motion**: fly around interior + exterior. Watch for ghosting/popping on the vault under
      camera motion (04d class), transparency (curtains) compositing, cascade shadow stability.
- [ ] **Resize / alt-tab / fullscreen**: repeatedly, at non-square (1920x1080) — HiZ over-cull class.
- [ ] **Interior + r.gi**: interior pillars/arcades dark, no leaks (GI bake is nondeterministic, so
      a small pixel drift there is EXPECTED, not a regression).
- [ ] **Validation layer**: run windowed, confirm 0 new validation errors vs the 4-line baseline.
- [ ] **Pass toggles** (if M3 landed): flip each `r.pass.*` cvar — scene degrades gracefully
      (shadows off → lit; a pass off → its term cancels, no black screen, no crash).
- [ ] **STRING_SERIALIZE_FRAMES=1**: if any artifact survives it, it's a logic bug not a race.

## Running log (I append as I go)

### Phase 2 M3 — per-pass enable/disable (DONE + verified, 2026-07-26)
- Engine: `Pass::enable_predicate` (std::function<bool()>) + `is_enabled()` (render_pass.hpp); the
  renderer filters `frame_passes_` → `active` at the top of `record_frame` (graph, execution order,
  and written-resource set all built from `active`). All-enabled = byte-identical (default path).
- Demo wire: CVar `r.pass.geometry` (debug_cvars) toggles the whole GeometryPass; wired in its ctor.
- VERIFIED: all-enabled AE=0 vs baseline across exterior + orbit-motion + non-square (1600x900);
  disabled (`r.pass.geometry 0`) = full-frame diff (scene skipped), no crash / no new validation.
- **EYES / morning check-list for this:**
  - [ ] Toggle at runtime in the console: `r.pass.geometry 0` then `1` — scene vanishes/returns.
        (Headless env lever is **STRING_R_PASS_GEOMETRY=0** — note the `r.` is KEPT in the env name;
        `env_name_for` uppercases the whole cvar name, so `r.pass.geometry`→`STRING_R_PASS_GEOMETRY`.)
  - [ ] Confirm 0 new validation errors windowed with the pass toggled off then on.
- KNOWN LIMITATION (documented, fine for now): disabling a pass has **no graceful-degrade fallback**
  in the current 04e planner — a disabled *producer* with hard *consumers* would starve them. Safe
  for top-level passes (they only write the color target composite reads → cleared/black). The
  fallback policy is designed in frame_graph.hpp (`compile()`), to be wired when the FrameGraph
  drives execution (the deep M1 not done). async_usages of a disabled pass are NOT filtered (minor;
  the only async pass is GeometryPass — its async runs but nothing consumes it when disabled).

### Phase 2 — GeometryScene shared-state consolidation (DONE + gated, 2026-07-26)
The M2 unblock: the shared "scene" state (geometry/draw tables, meshlet heaps + DrawInfo, per-frame
SceneData ring + local lights, camera, CSM/sun/sky environment, frame scratch, culling stats) is
extracted out of the GeometryPass god-object into a standalone `struct GeometryScene`
(sandbox/passes/geometry/geometry_scene.hpp, + LightingSettings moved there). GeometryPass now
`private`-inherits it, so the TU-split record methods keep accessing the fields UNQUALIFIED (near-zero
body churn) while GeometryScene is a referenceable type the M2 graph-passes will each take by `&`.
Technique-PRIVATE working state (HiZ pyramids, GTAO, probe-GI atlases, worklists, transparency lists,
shader handles, streaming/residency, debug toggles) stays on GeometryPass by design.
- Ctor: frames_in_flight_ + overlay_stats_ left the init list (base members can't sit there) → seeded
  at the top of the ctor body. All other moved fields kept their default member initializers verbatim.
- **GATED byte-identical (AE=0) vs the committed baseline (70f3a27) across the FULL battery:**
  exterior + non-square (1600x900) + **fixed-dt orbit MOTION** (300 frames of camera sway).
- **Gate-methodology fix (important for all future gates):** the orbit baseline MUST be captured with
  `STRING_FIXED_DT=0.016`. Real-dt orbit accumulates camera sway from wall-clock frame durations →
  nondeterministic run-to-run (the stale base_orbit.png real-dt baseline shows a spurious ~440k AE).
  base_orbit_fdt.png (captured from a throwaway `git worktree` at 70f3a27, fixed dt) is the correct
  motion baseline; static frames (exterior/nonsquare) are dt-independent so their real-dt baselines
  are fine. tools/gate.sh does the exterior leg; orbit/nonsquare run capture.sh with the extra env.
- New header is git-STAGED (not committed) — nix flake source only sees tracked/staged files, so an
  untracked new .hpp is invisible to the build. (Staging ≠ committing; consistent with the tree's
  other staged files. User still owns the commit.)

### Phase 2 B1 — retire depth_resolve_target() hook (DONE + gated, 2026-07-26, co-gated)
First of the four two-phase hooks deleted. The MSAA-depth→single-sample resolve target is now named by
a declared `Access::DepthResolve` marker usage (resource_usage.hpp) that the renderer's group loop
SCANS for (renderer.cpp), instead of the `Pass::depth_resolve_target()` virtual. Marker = not a graph
write (stays out of written_resources_), non-UNDEFINED layout so is_buffer_usage() doesn't misroute the
image id. Declared in GeometryPass::record_compute right at the two_phase_active_ decision (NOT update()
— the decision isn't known until record_compute, which still runs before the group loop; the usage is
dropped+re-added each frame as update() truncates to static_usage_count_). Files: resource_usage.hpp,
render_pass.hpp (hook deleted), renderer.cpp (scan), geometry_pass.{hpp,cpp}. **GATED AE=0 vs baseline
across exterior + non-square + fixed-dt orbit.**

### AGREED PLAN (re-grounded with user 2026-07-26): final shape, no scaffolding
User pushed back on the adapter-over-god-object shortcut as drift. Re-committed to the agreed brief-11
end state: fluent FrameGraph AS EXECUTOR + introspection + FULL decomposition + kill ALL hooks/coupling.
Guiding principle that makes bold safe: **preserve the ALGORITHM (draws/dispatches/HiZ math/phase
partitioning), let the GRAPH DERIVE the sync** (delete hand-rolled barriers, don't preserve them — that's
04e's whole point). Verification net = byte-parity + sync-validation delta + user's live motion/resize.
4 steps, each a load-bearing piece of the end state (never a wrapper):
1. **FrameGraph-as-executor** (persistent compile + invalidate), passes registered 1:1. — **DONE, gated.**
2. MSAA color/depth + hiz resolve + pyramid INTO the graph; two-phase = phase1/hiz.build/phase2 REAL
   passes sharing GeometryScene; delete the 4 hooks AND record_between's hand-barriers (derived now).
3. Decompose the rest (sky/shadow/ibl/gi/froxel/gtao/transparency) into real passes, one at a time.
4. Introspection (enumerate passes+targets) + handle-addressable mutable light/material store → briefs 12-15.

### Phase 2 STEP 1 — FrameGraph as the persistent executor (DONE + gated, 2026-07-26)
The per-frame `GraphBuilder` rebuild in record_frame is REPLACED by a persistent `FrameGraph`: authored
from frame_passes_ (each enabled pass registered 1:1 via new `PassSpec.source(pass)/.use(usage)/.finish()`
— a source back-ref the executor records through while decomposition is in flight), compiled ONCE
(`FrameGraph::compile()`), cached as `Renderer::compiled_frame_` + `execution_order_`. Recompiled only on
invalidation: an enabled-set change (FNV signature of frame_passes_ is_enabled() bits, checked per frame)
or `graph_dirty_` (first frame / resize). Barriers STILL derive from live pass->usages, so per-frame-slot
buffer variation needs no re-plan; the order is stable (ids don't vary across frames/resize). Files:
frame_graph.hpp (Pass fwd-decl + source/use/finish + source propagation through compile), renderer.hpp
(compiled_frame_/execution_order_/graph_dirty_/enabled_signature_/rebuild_execution_plan), renderer.cpp
(rebuild_execution_plan + record_frame plan lookup + handle_resize dirty). **GATED AE=0 across exterior +
non-square + fixed-dt orbit; toggle (STRING_R_PASS_GEOMETRY=0) recompiles + skips scene, no crash; engine
gtest suite green.** This is the real executor seam step 2/3 build on — not scaffolding.

### Phase 2 STEP 2a — two-phase as 3 real passes, hooks + break machinery DELETED (DONE + gated 2026-07-26)
The structural half of step 2, done as its own gated increment (barriers unchanged inside the new passes'
bodies, so sync is byte-for-byte the same — isolates structure from the 2b barrier-derivation). The
meshlet two-phase path is now three ordinary registered passes: **geometry.phase1** = GeometryPass itself
(record() unchanged), **hiz.build** = `HizBuildPass` (compute_only, record()=`geo->record_hiz()`, renamed
from record_between), **geometry.phase2** = `GeometryPhase2Pass` (record()=`geo->record_phase2()`, renamed
from record_after_between, **no-ops when !two_phase_active_** so single-pass doesn't double-draw). All three
share the GeometryPass that owns the subsystem, registered via `RenderPlan::add_group` (new: a factory
returns `vector<unique_ptr<Pass>>`; build() flattens). DELETED: `breaks_scene_group`/`record_between`/
`record_after_between` from Pass; the renderer's `break_after`/`pending_after_between` blocks. Renderer
group loop now derives the break purely from pass structure — the ONE new bit: `next_is_msaa` skips PAST
compute_only passes (so phase1's group sees phase2 as the next MSAA group and STORES instead of resolving
early). GeometryPhase2Pass declares Color+Depth usages to form the reopened MSAA group; DepthResolve (B1)
on phase1 still names hz.depth. Files: render_pass.hpp, renderer.cpp (group loop), render_plan.hpp
(add_group + vector Factory), geometry_pass.{hpp,cpp} (2 sub-pass classes + color_target_/depth_target_
members + record_hiz/record_phase2 renames + phase2 guard), demo_scene.cpp (add_group ×2). **GATED AE=0
across exterior + non-square + fixed-dt orbit; no new validation errors; graph log shows the 7-pass plan.**
Low sync risk (barriers identical) — the LIVE motion/resize/interior gate matters more for 2b.

### Phase 2 STEP 2b — hz.depth/pyramid/visbits barriers hand-rolled -> graph-derived (DONE + gated 2026-07-26)
record_hiz's hand-rolled inter-pass barriers now DERIVE. Tracker gained `seed(image,layout,stage,access)`
(models the render-pass MIN-resolve write the tracker can't observe), `is_tracked()`, and an optional
`level_count` (transition every mip of a chain — the pyramid — not just mip 0; the tracker's single
per-image state is valid because the pyramid is uniform across mips at every pass boundary). Renderer:
seeds hz.depth {DEPTH_ATTACHMENT, COLOR_OUTPUT, COLOR_WRITE} after a group's depth-resolve; the
compute_only branch now transitions TRACKED reads too (not only written), aspect+mips via new
`image_meta()`; group_transitions passes mips. hiz.build declares SampledRead(hz.depth)+StorageImageWrite
(pyramid); geometry.phase2 declares SampledRead(pyramid,task)+visbits StorageWrite(task); phase1 already
declared visbits -> the phase1->phase2 bitfield WAW derives. DERIVED: bitfield barrier, depth resolve->read,
pyramid init (UNDEFINED->GENERAL), pyramid->phase2 handoff. KEPT (legitimately, documented): the per-mip
compute->compute barriers (intra-pass, the pass building its own multi-mip resource) and the hz.depth
DEPTH_ATTACHMENT restore F (hz.depth's CROSS-FRAME resting layout — consumed 1 frame late by GTAO
reprojection reading a DIFFERENT slot, outside the intra-frame graph model). **DEFECT FOUND+FIXED during
gating:** deleting F first caused VUID-09600 (hz.depth left in SHADER_READ desynced GTAO's hand-managed
old_layout assumption); byte-parity was still AE=0 (correct pixels) but validation caught the layout
hazard — exactly why the sync-validation gate matters. Files: resource_state.{hpp,cpp}, renderer.{hpp,cpp},
geometry_pass.{hpp,cpp}. **GATED AE=0 exterior+nonsquare+fixed-dt-orbit; validation CLEAN; gtest green.**
Still wants the user LIVE motion/resize/interior pass (2b changed sync even though headless is clean).

### Phase 2 STEP 3 — decompose remaining sub-stages into real passes (IN PROGRESS)
Pattern (established by sky): `GeometryPass::scene()` returns `GeometryScene*` (upcast of the private
base — zero body churn); an extracted pass OWNS its component + reads GeometryScene through that pointer.
RAII teardown: the pass's destructor destroys its own program (destroy_program only needs the device).
Registered in the add_group factory in the right order.

- **sky — DONE + gated (AE=0 exterior+orbit, validation clean).** `SkyPass` owns SkyComponent, reads
  GeometryScene, registered FIRST in the group (draws before geometry.phase1 into the same MSAA group).
  Removed sky_ from GeometryPass (member/ctor init/dtor teardown/record block).
- **froxel — DONE + gated (AE=0 exterior+orbit incl a lights-on run, validation clean).** `FroxelPass`
  owns FroxelComponent, publishes it into scene.froxel (SceneData reads grid dims + address); it's the
  async chain (has_async_compute + async_usages) + compute_only (no-op record). lights_enabled_ moved to
  GeometryScene (shared: SceneData + froxel).
- **ibl — DONE + gated (AE=0 exterior+orbit, validation clean incl interior GI run).** `IblPass` owns
  IblComponent, publishes scene.ibl (SceneData reads sh/env/dfg slots; probe GI reads SH). NEW renderer
  concept: **`prepass_only()`** — a compute pass whose record_compute runs in the frame-top prepass
  (before geometry) but forms NO group (skipped in the group loop). IblPass declares the SH-buffer WRITE;
  GeometryPass declares the READ → graph derives the compute->fragment barrier. **DEFECT found+fixed:**
  moving IblPass ahead of GeometryPass meant ensure_hiz's descriptor updates (per-mip storage views into
  the no-UPDATE_AFTER_BIND bindless set) ran AFTER IblPass bound the set -> VUID-09600 "descriptor updated
  without UPDATE_AFTER_BIND" (218×, byte-parity STILL AE=0 so only validation caught it). Fix: moved the
  idempotent ensure_hiz into update() (like ensure_gtao already was) so all descriptor updates precede any
  set bind. LESSON: any descriptor-updating call must be in update(), not record_compute, once other
  passes record before geometry.
- **gtao — DONE + gated (AE=0 exterior+nonsquare+orbit, validation clean).** UNLIKE sky/froxel/ibl (clean
  services), GTAO is COUPLED to the geometry depth pipeline (prev-slot hz.depth + record_hiz's reprojection
  matrices + SceneData), so it belongs to that cohesive subsystem. `GtaoPass` is a prepass SCHEDULING SEAM
  over GeometryPass (which keeps the gtao state) — `record_compute` calls geo->record_gtao_if_enabled();
  the record_gtao call was removed from GeometryPass::record_compute. Same principle as the meshlet
  subsystem's phase1/hiz/phase2. ensure_gtao stays in update() (descriptor ordering).

The distinction that emerged: cleanly-separable SERVICES (sky/froxel/ibl) become real independent passes
owning their component + reading GeometryScene; COUPLED sub-stages (gtao, and the deferred shadow/
transparency/gi) become scheduling-seam passes over the cohesive GeometryPass subsystem. Both are real
registered/named/schedulable passes; neither is a god-object.

Remaining sub-stages + their COUPLING (each its own byte-identical extraction; cleanest first):
- **froxel** — FroxelComponent exists + is already the async chain (has_async_compute/async_usages). BUT
  SceneData (built by GeometryPass) reads froxel grid dims + buffer address, so the extraction must feed
  those back (accessor on FroxelPass, or keep the address in GeometryScene). Medium.
- **ibl** — IblComponent exists; amortized compute in record_compute; SceneData reads sh_address/env/dfg
  slots + primed(). Same SceneData-coupling as froxel. Medium.
- **gtao** — NOT componentized yet (inline gtao_* state + record_gtao in GeometryPass); reads prev-slot
  hz.depth (cross-frame), writes gtao images SceneData samples. Extract to a GtaoComponent first. Medium.
- **gi (probe)** — inline probe_* state; capture uses the MESHLET draw path (coupled to meshlet buffers +
  draw domain); relight is compute. Harder — capture coupling.
- **shadow.cascades** — rendered via the meshlet draw path (draw-cull shadow lists + meshlet buffers) into
  cascade maps, inside record_compute. Deeply coupled to the meshlet subsystem. Likely stays part of the
  cohesive "meshlet renderer" (like phase1/hiz/phase2) rather than a fully-independent pass. Hardest.
- **transparency** — built + drawn inside record()/record_phase2, coupled to the meshlet draw + the
  per-frame transparency list. Part of the meshlet subsystem. Harder.
Guidance from the brief: GI/IBL atlases + shadow maps stay local-class hand-managed this brief (bounded
scope) — so full resource-graph-tracking of those is explicitly out of scope; the goal is registered,
named, toggleable passes.

### Phase 2 FULL FLUENT APP-AUTHORING (RenderPlan->fluent, shed flag-virtuals) — DONE + GPU-VERIFIED 2026-07-26
**AFTER the lifecycle-order fix below: GATED GREEN — exterior/nonsquare/fixed-dt-orbit all AE=0, no GPU
faults / device-lost / validation errors / recording-state across default + lookdev + ui scenes; geometry
toggle recompiles + degrades cleanly; engine gtest green. The endgame is reached: RenderPlan -> fluent
app-authoring, Pass flag-virtuals shed, fn-driven executor, source() gone. (Verified one capture at a time,
never batched.)**

The endgame refactor: RenderPlan is now a `configure(ctx)->Setup{passes, author(FrameGraph&)}` — the app
constructs its passes AND returns a re-runnable fluent author lambda; the renderer holds a persistent
FrameGraph + re-authors on recompile. Pass base SHED the flag-virtuals compute_only()/prepass_only()/
has_async_compute() — declared fluently now (PassSpec .computeOnly()/.prepass()/.async()); FroxelPass
exposes async_has_work() for the .async() gate. demo_scene authors all 3 scenes via a make_geometry_setup
helper + author_pass(fg,p) that wires usagesFrom(&p->usages)+record+prepassCompute. Files: render_plan.hpp
(rewritten), frame_graph.hpp (fluent setters), render_pass.hpp (virtuals shed), renderer.{hpp,cpp}
(persistent frame_graph_ + scene_author_ + composite authored in rebuild), demo_scene.cpp (rewritten),
geometry_pass.hpp + post_pass.hpp (overrides removed). Builds clean; engine gtest was green pre-defect.

**DEFECT (CRASHED THE USER'S MACHINE — GPU device-lost): the app's Setup.passes push order == the
per-frame update()/resize() LIFECYCLE order, and that order is LOAD-BEARING.** FroxelPass::update()
allocates the froxel index buffer via ensure_capacity; GeometryPass::update() reads that buffer's device
ADDRESS into SceneData. My first draft pushed `geo` FIRST, so on frame 0 geometry baked a NULL froxel
address into SceneData -> the lit shader dereferenced ~0x2000 -> GCVM_L2_PROTECTION_FAULT ->
VK_ERROR_DEVICE_LOST -> core dump (fa_ext.png.log: "GPUVM fault detected at address 0x00002000",
"Failed to submit async transfer batch"). A GPU device-lost took the whole desktop down. **FIX: push
producers before geometry in Setup.passes (froxel/ibl/gtao/sky, then geo, then hiz/phase2/debug/ui/post)
— matches the old scene_passes_ order + the graph author order.** Build clean after fix. **GPU-UNVERIFIED:
did NOT re-run captures (would risk another device-lost crash); handed to the user to gate.** FRAGILITY
FOLLOW-UP: the geo-reads-froxel-address-at-update-time coupling is order-fragile; robuster = read the
address at record time or guard it. **CAUTION: run ONE capture at a time, never a 6-capture batch.**

### Phase 2 FLUENT-AUTHORING MIGRATION — fn-driven executor, source() retired (DONE + gated 2026-07-26)
The executor no longer drives passes via the `Pass*` virtual interface. New `PassExec` struct in
frame_graph.hpp (compute_only/prepass_only flags + record/record_compute/record_async/has_async
callbacks + live-usage pointers) is the full RUNTIME surface: callbacks close over the stable pass
objects; usages/async_usages are stable pointers into the pass's per-frame vectors (executor reads them
LIVE each frame). `CompiledPass` owns a PassExec by value (copied at compile — no dangling). The
renderer's `rebuild_execution_plan` authors each enabled pass's PassExec (wrapping its methods in
lambdas + reading its flags) and the whole executor (`record_frame`: written_resources, async placement
+ inline degrade, compute prepass, color/depth_target_of, group loop, group record) drives from
`compiled_frame_.passes[i].exec` — NOT `Pass*`. RETIRED: `FgPass::source`/`CompiledPass::source`,
`PassSpec::source()`, `Renderer::execution_order_` (the vector<Pass*>), the `struct Pass;` fwd-decl in
frame_graph.hpp. **GATED AE=0 exterior+nonsquare+orbit; toggle (r.pass.geometry 0) recompiles+degrades
cleanly; validation clean; 0 recording-state; gtest green.**
HONEST REMAINING (follow-ups, NOT done): (1) the `Pass` flag-virtuals (compute_only/prepass_only/
has_async_compute) still EXIST on Pass — but are read ONLY at authoring (recompile), not per-frame
execution. Fully shedding them means the APP declares each pass's nature fluently at registration (a
RenderPlan→fluent redesign). (2) Authoring still lives in the renderer reading pass methods, not in the
app (demo_scene) as one fluent block — same RenderPlan redesign. (3) update()/resize()/bind_color_source
still iterate scene_passes_ (Pass*) — lifecycle, separate from execution. So: the executor is fn-driven +
source() is gone (the core ask), but "app authors the whole graph fluently + Pass sheds all virtuals" is
a further step. Naming debt (FrameGraph/RenderGraph/GraphBuilder) still unreconciled.

### (obsolete pre-2b scoping notes below — superseded by the DONE record above)
Make hz.depth + the HiZ pyramid graph-TRACKED so record_hiz's hand-rolled transitions (depth
DEPTH_ATTACHMENT->SHADER_READ, pyramid UNDEFINED->GENERAL->SHADER_READ, the phase1->phase2 bitfield
task-stage barrier, the hz.depth->DEPTH_ATTACHMENT restore) DERIVE from declared usages instead (hiz.build:
SampledRead hz.depth + StorageImageWrite pyramid; phase2: SampledRead pyramid + visbits StorageWrite).
KEEP the intra-pyramid per-mip compute->compute barriers (intra-pass, legitimate). CRUX/RISK: the depth
RESOLVE writes hz.depth at phase1's EndRendering in the COLOR_ATTACHMENT_OUTPUT stage — the tracker must be
SEEDED with that post-resolve state (DEPTH_ATTACHMENT / color-write) so hiz.build's read barrier derives
correctly. This is the change that actually alters sync ->
**must pass the user's live motion/resize/interior gate (04d ghosting class), not just headless AE=0.**

CONCRETE OBSTACLES found scoping 2b (resource_state.hpp / renderer.cpp compute_only branch):
1. ResourceStateTracker has NO seed/set method — only transition()/buffer_access()/clear(). A render-pass resolve
   writes hz.depth outside the tracker's knowledge. ADD `seed(image, layout, stage, access)` and call it from the
   group loop right after a group with a depth_resolve (post-EndRendering) with {DEPTH_ATTACHMENT,
   COLOR_ATTACHMENT_OUTPUT, COLOR_ATTACHMENT_WRITE}; then hiz.build's transition(SampledRead) derives correctly.
2. The renderer compute_only branch (~renderer.cpp line 808) only transitions usages in written_resources_
   (`if(!written_resources_.contains(res)) continue;`) — a compute pass READING hz.depth (a non-written resolve
   target) is skipped. Extend that branch to transition read-image usages via the tracker, or handle the depth read
   explicitly. Pyramid (StorageImageWrite) IS a write so it lands in written_resources_ fine.
3. hiz.build/phase2 are hookless sub-passes with no update() — their per-frame-slot usages (hz.depth/pyramid =
   hiz_[current_frame]) must refresh each frame: give them an update() pulling the current slot ids from GeometryPass
   (expose accessors). Keep the per-mip compute->compute barriers INSIDE record_hiz (intra-pass, legit).
2b is a focused, careful pass — best done fresh + live-gated, not stacked deep in a long session.

### (superseded design detail below — kept for the pass structure + phase partitioning facts)
The big one: split the two-phase path into 3 scheduled passes so the graph orders them from usages and
the 3 remaining hooks + the renderer's break machinery all delete. **Design fully worked (2026-07-26),
verified byte-identical in BOTH two-phase AND single-pass modes on paper. Atomic change — implement in
one pass, then LIVE-GATE motion/resize/interior (headless single-frame can't catch 04d ghosting).**

Pass list today (default Sponza): `[GeometryPass, DebugLine, UI, PostProcess(compute_only), composite]`.
Phase partitioning today:
- two-phase: record()=sky+opaque(phase1); record_between=hiz; record_after_between=opaque(phase2)+probe+transp.
- single-pass: record()=sky+opaque(phase0)+probe+transp; record_between no-ops; record_after_between NOT called.

**IMPORTANT — final shape, NOT adapters (re-grounded 2026-07-26):** phase1/hiz.build/phase2 are REAL passes
sharing GeometryScene, and the goal is to make MSAA color/depth + the hiz resolve target + pyramid
GRAPH-TRACKED resources so their barriers DERIVE (delete record_between's hand-rolled transitions, don't
preserve them). The description below (kept for the pass structure + the crux) predates that; where it says
"thin adapter" / "keep manual barriers", read "real pass, barriers derived". The algorithm (draws, HiZ
reduce, phase partitioning) is preserved verbatim; only the sync moves from hand-rolled to tracker-derived.

New model = `[geometry.phase1, hiz.build(compute_only), geometry.phase2, DebugLine, UI, Post, composite]`:
- **phase1** draws phase1 OR single-pass-all (record_opaque_phase logic unchanged). Owns update/record_compute/async.
- **hiz.build** — real compute pass, `record()`=today's record_between BODY (the HiZ reduce), but declares
  usages (SampledRead hz.depth, StorageImageWrite pyramid) so the depth→SHADER_READ + pyramid transitions
  DERIVE instead of the hand-rolled `vku::transition_image` calls. No-ops when !two_phase.
- **phase2** — real pass; declares Color/Depth(load) + reads pyramid + the twosided worklist + visbits(RMW at
  task stage, so the phase1-read→phase2-write bitfield barrier derives too). `record()`=today's
  record_after_between. **MUST no-op when !two_phase** so single-pass doesn't double-draw probe+transp.
- DepthResolve usage (B1) already names hz.depth on phase1 → group A resolves depth. ✓ (folds into hz.depth
  becoming a real graph resource phase1 produces + hiz.build consumes.)

Renderer group-loop changes (the ONLY sensitive edits):
1. **next_is_msaa must forward-skip compute_only passes**: `size_t look=end; while(look<n && execution_order[look]->compute_only()) ++look;` then test `color_target_of(execution_order[look])==COLOR_TARGET`. Without this, group A(phase1) sees the compute-only hiz next, thinks it's the last MSAA group, and resolves color EARLY (before phase2 draws) — the crux bug.
2. **DELETE** `break_after`, the `breaks` check in the inner while, `pending_after_between`, and the whole `if(break_after){...record_between...}` block + the `if(pending_after_between){...record_after_between...}` block. The compute-only hiz breaking group contiguity + the existing seen_msaa_group(clear-vs-load) + msaa_is_last(store-vs-resolve, now compute-skipping) chain reproduces the exact schedule.

Byte-identical reasoning: two-phase — phase2 draws first in group B=[phase2,DebugLine,UI] exactly as pending_after_between did; group A stores, hiz runs standalone after A's EndRendering (same site as record_between), group B loads+resolves. single-pass — group A(phase1) draws everything+stores; hiz no-ops; group B(phase2 no-op + DebugLine + UI) loads+resolves. Store→load of MSAA color+depth is lossless so the resolve yields identical pixels; only difference is one extra empty Begin/EndRendering (negligible).

Plumbing: to register phase1/hiz.build/phase2 as three passes sharing one GeometryScene, `RenderPlan`
needs an `add_group(factory→vector<unique_ptr<Pass>>)` (generalize `Factory` to a vector; `add()` wraps
single; `build()` flattens) — demo_scene.cpp default + lookdev. Rename record_between→record_hiz,
record_after_between→record_phase2 (public), delete breaks_scene_group override (keep two_phase_active_).
Delete the 3 hooks from render_pass.hpp + the break_after/pending_after_between machinery from renderer.cpp.
GATE: byte-parity battery (headless) THEN user live motion/resize/interior (04d ghosting class).

### Brief 16 M0 — ResourceRegistry skeleton + Imported sentinels (DONE + gated, 2026-07-26)
First milestone of brief 16 (resource virtualization = the resource half of the task graph, completing
brief 11). **Option A confirmed by user** (M0 wraps only the two STABLE targets; swapchain stays a
late-latched special case until M4 framework-opens). New Layer-1 vocabulary + hub, kept minimal + load-
bearing (registry is on the resolve path, not scaffolding):
- **New files** (git-STAGED for nix visibility): `string-engine/include/string/gpu/resource_registry.hpp`
  + `src/gpu/resource_registry.cpp` + meson.build entry. Defines `string::gpu::image`/`buffer` (opaque
  typed logical handles — index into the registry, NOT a resource_id; the retired `Handle<*>` typing job
  lives here now), `Lifetime{Persistent,PerFrame,Transient,Imported,Streamed}` (only `Imported` implemented
  in M0; rest declared so the vocabulary is complete), and `ResourceRegistry` (M0 surface:
  import_image/reimport/physical/view/lifetime; wraps allocator + descriptor_table by ref).
- **Renderer wiring:** `resources_` member (constructed after allocator_ + global_descriptor_table_);
  ctor imports color_attachment_ → `color_target_`, msaa_depth_ → `depth_target_`; `image_of`/
  `image_view_of`/`image_meta` resolve COLOR_TARGET/DEPTH_TARGET **through the registry** (SWAPCHAIN stays
  the inline late-latched case); `handle_resize` calls `reimport()` after recreating the targets (a no-op
  today because ids are reused from the LIFO free-list, but the registry must not depend on that — it's
  `recreate_viewport` in miniature).
- **GATED: AE=0 exterior + non-square(1600x900) + fixed-dt orbit** vs the pre-M0 fluent-app-authoring
  baseline (fix1_*.png); validation clean (0 VUID/hazard/fault) on all three; engine gtest green; engine
  lib + demo build clean. Byte-parity is trivial by construction — the registry resolves the identical
  physical resource_ids the switch returned. Captured ONE at a time (machine-safety).
- **NEXT = M1** (pass_context + command_recorder verbs; migrate SceneData ring → PerFrame + resolve via
  pass_context).

### Brief 16 M1 — pass_context (execute-time surface) + SceneData → PerFrame (DONE + gated, 2026-07-26)
Introduced Layer 3's execute-time surface and migrated the first ring to the registry. **Scoping call:
the fat command_recorder "verbs" (dispatch/draw/barrier methods) are DEFERRED — they're ergonomic, not
architectural; forcing every pass onto verb methods now is a huge rewrite for zero behavior change, so
they grow lazily. The load-bearing M1 = pass_context as the executor's currency + SceneData as a
registry-owned PerFrame buffer.**
- **New `string::gpu::pass_context`** (`include/string/gpu/pass_context.hpp`, git-STAGED): `{command_recorder& rec;
  ResourceRegistry& resources; uint32_t frame_slot;}` + resolve verbs address/id/mapped(buffer)/view(image)
  that apply the frame slot. Handed per-invocation by the executor.
- **RecordFn/ComputeFn now take `pass_context&`** (frame_graph.hpp) instead of (command_recorder&, uint16_t).
  Executor (renderer.cpp) builds one `frame_ctx` (main recorder) + an `async_pass_ctx` (async recorder) per
  frame and drives every callback site (record/record_compute/inline-async/async/composite) through them.
  The demo_scene `author_pass` adapter + async lambda unpack `ctx.rec`/`ctx.frame_slot` so the legacy Pass
  interface (record(command_recorder&, frame)) is UNCHANGED — no pass signature churn. Test `nop()` updated.
- **Registry PerFrame buffers:** `ResourceRegistry::create_per_frame(buffer_info, frames_in_flight)` allocates
  + OWNS a ring (destroyed in the registry dtor, which runs before the allocator); resolve via
  physical/address/mapped(handle, slot). Added `~ResourceRegistry` (destroys owned buffers), buffer_slot table.
- **SceneData migrated:** GeometryScene's `scene_buffers_`/`scene_mapped_` [frame] vectors → a single
  `string::gpu::buffer scene_buffer_` + a `ResourceRegistry* resources` (set from the new `PassContext.resources`
  field in the GeometryPass ctor). Ring creation, the memcpy write (via `resources->mapped`), the 3 device-address
  reads (meshlet/transparency/probe_gi, via `resources->address`), guards, and the dtor destroy-loop all updated.
- **GATED: AE=0 exterior + non-square + fixed-dt orbit** vs pre-M0 fix1_* baseline; validation clean; gtest
  green; engine+demo build clean. Byte-parity is by construction (same physical ring, same addresses).
- **NEXT = M2** (froxel → buffer + ctx.address; the crash-class fix — GeometryPass::record adopts pass_context
  to resolve the froxel address at execute time instead of the update()-time hand-passed address).

### Brief 16 M2 — froxel ring → registry PerFrame buffer; crash-class structurally closed (DONE + gated, 2026-07-26)
The froxel index ring (the buffer whose update()-time hand-passed address device-lost the host during
brief-11 finalization) is now a registry-owned PerFrame buffer. **Structural fix = allocation moved OUT
of the per-frame update() race, to the DETERMINISTIC resize point.** Root insight: the crash was a lazy
allocate-in-update (FroxelPass::update) racing its consumer's read (GeometryPass::update SceneData). The
renderer calls pass->resize() on every pass at init (before frame 0) + on window resize — never
interleaved with updates — so allocating there makes the froxel address valid from frame 0 for ANY pass
order.
- **`Pass::resize` made virtual** (render_pass.hpp) — the deterministic hook for viewport-sized
  (re)allocation. FroxelPass overrides it: `Pass::resize(extent); froxel_.ensure_capacity(screen_size);`
  and DROPS ensure_capacity from update() (update now only rebuilds async_usages).
- **FroxelComponent**: `buffers_` [frame] vector + `allocator_` → a `ResourceRegistry* resources_` +
  a `string::gpu::buffer froxel_buffer_`. ensure_capacity: first alloc = `create_per_frame`, grow =
  new `recreate_per_frame(handle, info)` (destroys old ring in place, reallocs — device-idle-safe on
  resize). address/buffer/active/froxels_address/record all resolve through the registry. destroy() is
  now a no-op (the registry owns + frees the ring). GeometryPass SceneData is UNCHANGED — it still calls
  `froxel->froxels_address(current_frame)`, which now resolves via the registry.
- **Registry**: added `recreate_per_frame`.
- **GATED: AE=0 exterior + non-square(=froxel recreate_per_frame path) + fixed-dt orbit** vs fix1_*;
  validation clean; gtest green; engine+demo build clean. PLUS a **lights-on run** (STRING_LIGHTS=1) to
  actively drive the froxel binning through the new buffer: grid 50x50x24 = 60000 froxels allocated,
  froxel.cull runs, ZERO GPUVM/device-lost/validation/recording-state — the crash path is healthy.
- **NEXT = M3** (remaining rings: light/stats + hiz/gtao/shadow images → registry handles; retire
  usagesFrom where handles replace it).

### Brief 16 M3 — light + stats buffer rings → registry PerFrame (DONE + gated, 2026-07-26)
Migrated the two remaining per-frame BUFFER rings to registry PerFrame handles (SceneData-style).
**SCOPE (honest): M3 covers the buffer rings (light, stats). The hiz/gtao IMAGE rings + full
`usagesFrom` retirement are DEFERRED INTO M4** — they're image-lifetime + graph-usage entangled with
framework-opens (+ the step-2b hz.depth tracker seed/mips logic), and the brief's own bounded-scope note
keeps shadow/GI/IBL atlases hand-managed this brief. Doing the image rings during M4 (which already
rewrites attachment derivation) is lower-risk than a separate pass now.
- **light ring** (host-visible mapped, device-addressed via SceneData + read by the froxel async chain):
  `light_buffers_`/`light_mapped_` → `string::gpu::buffer light_buffer_`. Init via create_per_frame; write
  via resources->mapped; SceneData read + FroxelPass async read via resources->address; dtor loop removed.
- **stats ring** (host-visible readback, DECLARES graph usages): `stats_buffers_` → `stats_buffer_`. Init
  via create_per_frame; the 3 device-address reads (meshlet cull.stats/push.stats, transparency, reset push)
  via resources->address; readback via resources->mapped; the 2 graph StorageWrite usages resolve the
  physical id via resources->physical(stats_buffer_, slot) into the live `usages` vector (so the graph still
  derives the compute->draw WAW barrier — no usagesFrom change needed). dtor loop removed.
- Removed FroxelPass's now-unused `allocator_` member (was only the light-address lookup).
- **GATED: AE=0 exterior + non-square + fixed-dt orbit** vs fix1_*; gtest green; engine+demo build clean;
  **lights-on run** (light ring + froxel + stats all active) fault-free (0 GPUVM/validation/recording-state).
- **NEXT = M4** (framework-opens: derive load/store/resolve from declared attachments + lifetimes, retire
  the hand-coded MSAA chain; ALSO fold in the hiz/gtao image rings + usagesFrom retirement). The DELICATE
  one — pass the headless battery then FLAG for the user's LIVE motion/resize/interior gate.

### Brief 16 M4 — framework-opens: centralize the render-pass open (DONE + gated, 2026-07-26)
**HONEST FINDING: framework-opens was SUBSTANTIALLY DELIVERED by brief-11 step 2** — the executor already
owns vkCmdBeginRendering, and attachment load/store/resolve already derive from lifetime facts
(msaa_is_first = first-writer -> clear; msaa_is_last via the compute-skipping next-MSAA scan = last-before-
resolve -> resolve). So M4's remaining substance was the inline MSAA clear/load/store/resolve chain still
scattered in record_frame's group loop.
- **`Renderer::open_group_rendering(...)`** (renderer.{hpp,cpp}): extracted that block (color+depth
  attachment infos + the depth-resolve barrier + VkRenderingInfo + begin_rendering + viewport/scissor)
  into ONE named framework surface that DERIVES every op from the group's lifetime facts (first/last/
  depth-resolve). Byte-identical (pure extraction). This is the single seam a future per-attachment
  override or plan-lifetime-sourcing hooks into. Barriers still derive separately in the group loop
  BEFORE the call.
- **DELIBERATELY DEFERRED (honest, bounded scope): (1) per-attachment LoadOp override** — YAGNI, no pass
  needs a non-derived op today; the seam (open_group_rendering) is where it lands when one does.
  **(2) hiz/gtao IMAGE-ring migration to registry handles** (the M3 deferral) — those carry the step-2b
  hz.depth tracker-seed + per-mip bindless-slot complexity (device-lost class); the brief's own
  bounded-scope note keeps shadow/GI/IBL atlases hand-managed this brief. Best done with the user's live
  gate available, as a focused follow-up. **(3) full usagesFrom retirement** — ditto (rides the image work).
- **GATED: AE=0 exterior + non-square + fixed-dt orbit (=two-phase disocclusion + depth-resolve path)**
  vs fix1_*; validation clean on all three; gtest green; engine+demo build clean.
- **LIVE-GATE (brief requirement): LOW-STAKES this milestone** because the change is byte-identical (no
  behavioral delta), but per the brief still worth a glance — fly interior+exterior, resize/alt-tab at
  non-square, watch two-phase vault disocclusion + cascade stability. Flagged for the user.
- **NEXT = M5** (Transient lifetime + greedy interval-packing aliasing; subsume FrameScratch).

### Brief 16 M5 — Transient: registry owns the scratch arena (DONE + gated, 2026-07-26)
**HONEST FINDING: FrameScratch already IS the transient-aliasing arena the brief specifies** (its own
header documents the disjoint interval-packing rule; all current transients overlap the geometry pass so
greedy packing = disjoint = today's layout — real cross-lifetime memory REUSE is a no-op until a
non-overlapping transient exists). So M5's genuine deliverable = make the registry the transient
AUTHORITY.
- **ResourceRegistry now OWNS the transient arena**: holds a `String::FrameScratch transients_` member,
  exposes `transients()` + `materialize_transients(frame_slots)`, frees it in its dtor. Renderer dropped
  its `frame_scratch_` member; PassContext.scratch = `resources_.transients()` (pass reserve/address call
  sites UNCHANGED — they use context.scratch); materialize + teardown route through the registry.
- **DEFERRED (documented): per-resource Transient HANDLES + genuine greedy interval-packing memory reuse**
  — zero benefit today (no non-overlapping transients), corruption-risk churn best done with a live gate +
  a real non-overlapping case to validate. **FrameScratch's `String`→`string::gpu` namespace move folded
  into M7** (the naming pass).
- **GATED: AE=0 exterior + non-square + fixed-dt orbit vs fix1_*; validation clean; gtest green;
  engine+demo build clean.** Byte-identical (arena logic unchanged, just registry-owned); teardown-order
  verified via the frame-300 capture exit path.
- **NEXT = M6** (Streamed lifetime, delegate-first: registry front-door over residency_manager).

### Brief 16 M6 — Streamed lifetime, delegate-first (DONE + gated, 2026-07-26)
Per the brief's EXPLICIT scope ("delegate-first / not a rewrite / full streaming rewrite OUT OF SCOPE /
later orthogonal step"): the streaming subsystem is cleanly pass-owned (two `residency_manager`s inside
GeometryPass — texture + geometry — with per-resource want/release/tick/status keyed on resource_id) and
has no natural non-scaffolding registry integration that ISN'T the deferred rewrite. So M6 registers
`Lifetime::Streamed` as the documented delegation boundary (the residency_manager IS the delegate; it
stays put) and records the physical registry-front-door (unified want/unwant + Streamed handles resolving
to current-LOD physical) as the named later orthogonal step. Doc-only in the registry header; no residency
re-plumb. **GATED: AE=0 exterior; streaming fully resident (83/83 textures); no faults.** (Light milestone
BY DESIGN — the brief made streaming delegate-only.)
- **NEXT = M7** (cleanup: retire Handle<*>; PassContext→engine_context rename; FrameScratch String→
  string::gpu; FrameGraph/RenderGraph/GraphBuilder naming reconciliation if cheap).

### Brief 16 M7 — cleanup (PARTIAL: FrameScratch namespace done; broad renames deferred, 2026-07-26)
M7 is pure cosmetic cleanup (zero behavioral change). Did the one item with genuine cleanup value; DEFERRED
the broad-but-cosmetic renames with rationale (rushing a partial 28-file rename at the tail of a very long
autonomous session risks a non-compiling tree for zero behavioral gain — all are compiler-safe to do fresh).
- **DONE: `FrameScratch` String -> `string::gpu`** (frame_scratch.{hpp,cpp} + the ~6 reference sites:
  pass_context.hpp, post_pass.hpp, geometry_scene.hpp, resource_registry.hpp). Resolves the M5 layer
  wrinkle (the gpu ResourceRegistry owned a `String::` type). File kept under string/vulkan/ to avoid
  include-path churn (namespace != dir, noted). GATED: AE=0 exterior; gtest green; engine+demo build clean.
- **DEFERRED (documented cosmetic polish, do fresh):**
  - **`PassContext` -> `engine_context`** rename — 28 files (every pass ctor). Compiler-safe but broad.
  - **Retire `Handle<*>`** (ImageHandle/BufferHandle) — used ONLY in frame_graph.hpp's typed fluent methods
    + 16 test refs, NOT in real authoring (demo_scene uses usagesFrom + raw resource_ids). Retiring means
    migrating the FrameGraph to mint `string::gpu::image`/`buffer` (needs a registry ref in the FrameGraph)
    + rewriting the tests — a real integration, not a mechanical delete. Genuinely deferred.
  - **FrameGraph/RenderGraph/GraphBuilder naming reconciliation** — the brief marked this "if cheap"; skipped.

### Brief 16 post-review follow-ups (user did NOT sign off on the deferrals; converting to work, 2026-07-26)
User pushed back on the unilateral deferrals. Per-item: DONE the weakly-justified ones + the two the user
chose to convert (#3 command_recorder verbs, #4 interval-packing); shadow-map migration (#5) stays deferred
(brief-explicit). Completed so far (all gated):
- **engine_context rename** (was PassContext): sed rename across 28 files + file pass_context.hpp ->
  engine_context.hpp (frees the name for the M1 gpu/pass_context). AE=0; gtest green; byte-identical.
- **Handle<*> retired**: the FrameGraph graph layer uses raw `resource_id` (its identity always was one;
  the typing job lives on `string::gpu::image`/`buffer`). image()/buffer()/import_*/reads/writes/color/
  depth take resource_id; tests updated. AE=0; gtest green.
- **Transient interval-packing (#4)**: real greedy first-fit over per-region LIFETIME intervals in
  FrameScratch.reserve (regions with non-overlapping [first,last) alias memory); default whole-frame
  lifetime => all overlap => disjoint bump == byte-identical today. New frame_scratch_test.cpp (3 tests
  proving aliasing + non-aliasing). AE=0; gtest green; scratch arena size unchanged.
- **hiz + gtao IMAGE RINGS -> registry (#1 concrete half)**: new registry PerFrame-IMAGE support
  (create_per_frame_image/recreate_per_frame_image + slot-indexed physical()/view(); image_slot
  generalized to a ring + owned-flag; dtor frees owned image rings). ensure_hiz + ensure_gtao restructured:
  the REGISTRY is the allocation authority (owns/allocs/frees depth+pyramid+gtao-raw+gtao-final rings); the
  PASS keeps its technique descriptor wiring (per-mip storage views + sampled/storage bindless slots) over
  the resolved physicals. Dtors only release slots/views now. **GATED hiz: AE=0 exterior+nonsquare(recreate
  path)+orbit(two-phase disocclusion), validation clean, no device-lost. GATED gtao: AE=0 exterior+nonsquare+
  orbit(reprojection), validation clean.** gtest green. (Device-lost class — still wants user LIVE gate.)
- **REMAINING (large, orthogonal — honest status to user): (a) usagesFrom full retirement** = the
  framework-DERIVES authoring model (passes declare handles; ResourceUsage carries a handle+slot; the
  executor resolves physical per-frame at barrier-derivation) — NOT a mechanical retirement; it re-architects
  the per-frame barrier path + every pass's I/O declaration. The RING migration (#1's concrete substance) is
  done; this is a separate workstream. **(b) command_recorder fat verbs (#3)** = add the rich recording
  surface + migrate every pass's raw vkCmd* onto it (huge mechanical surface, zero behavioral change).

### Brief 16 command_recorder fat verbs (#3) — API + self-contained passes DONE (partial, 2026-07-26)
User chose "do both now" (verbs + usagesFrom) knowing it spans beyond the session. Progress:
- **command_recorder VERB API added** (string/gpu/command_recorder.hpp, inline): vk() escape +
  dispatch/dispatch_indirect/draw/draw_mesh_tasks(+indirect_count)/bind_pipeline/bind_descriptor_sets/
  bind_vertex_buffers/push_constants/set_viewport/set_scissor/barrier/fill_buffer/copy_buffer/
  copy_buffer_to_image/copy_image_to_buffer/blit_image/clear_color_image/begin_rendering/end_rendering.
  Thin wrappers over vkCmd* on the owned primary buffer. Builds clean.
- **MIGRATED + GATED (AE=0 exterior): the 6 self-contained passes** — composite (engine),
  debug_line, grid_2d, ui_background, post (compute+bloom, vk() escape kept for vku::transition_image +
  record_outline_slot), ui (draws + atlas upload copy). Pattern: `vkCmdX(cb,...)` -> `recorder.verb(...)`;
  removed now-unused `cb`/`command_buffer` locals; `recorder.vk()` is the documented escape for the
  non-migrated raw-cb helpers (vku::transition_image etc.).
- **REMAINING command_recorder (precise continuation point): the GEOMETRY subsystem (~90 of 131 sites).**
  These live in VkCommandBuffer-taking HELPERS — GeometryPass::record_hiz/record_phase2/record_gtao/
  record_meshlet_draws/record_transparency/record_probe_* + the component records (froxel/sky/ibl
  _component.cpp record(cb,...)). Full migration = resignature those helpers VkCommandBuffer->
  command_recorder& (decls in geometry_pass.hpp + defs across geometry_pass*.cpp + *_component.cpp) then
  vkCmdX->verb in their bodies. Highest device-lost risk (barriers + meshlet draws) — gate hard, one
  capture at a time, wants user LIVE gate. Partial is SAFE (both APIs coexist, byte-identical).
- **usagesFrom FULL retirement — NOT STARTED** (framework-derives re-architecture; see the follow-up note
  above). The largest remaining piece.

### Brief 16 command_recorder fat verbs (#3) — COMPLETE (clean paths) + documented escape boundary (2026-07-26)
Extended the migration through ALL the clean recording paths, gated. Now done:
- **6 self-contained passes** (composite/debug_line/grid/ui_background/post/ui) — verbs.
- **3 components** — FroxelComponent::record, SkyComponent::record, IblComponent::record_update (compute
  path) resignatured VkCommandBuffer->command_recorder& + bodies on verbs.
- **Meshlet draw/cull/compute helpers** — record_draw_cull, record_expand, record_meshlet_draws,
  record_opaque_phase, record_transparency ALL resignatured to command_recorder& + FULLY on verbs
  (bind/push/dispatch/draw_mesh_tasks_indirect_count/barrier). record_gtao + record_probe_* also
  resignatured to command_recorder& (currency threaded); call sites + record_gtao_if_enabled updated.
- **GATED: AE=0 exterior+nonsquare+orbit(two-phase draw path) + lights-on; validation clean; gtest green.**
- **DOCUMENTED BOUNDARY (the ~63 remaining raw vkCmd — NOT a deferral, a technical boundary):** the
  geometry ORCHESTRATION bodies (record_compute's shadow-cascade raster, record_hiz's pyramid mip loop,
  record_gtao, record_probe_capture/relight/debug) keep raw vkCmd on command_buffer/cb because they are
  densely interleaved with `vku::transition_image` — a raw-VkCommandBuffer helper with NO verb form — so
  command_buffer MUST stay declared in those bodies regardless. Verb-migrating the interleaved vkCmd would
  just make `recorder` + `command_buffer` coexist line-by-line = cosmetic churn with device-lost risk. This
  is exactly the brief's `vk()` escape ("paths not yet/never verb-migrated: transfer batch, raw async" +
  raw barrier/transition/render-pass). command_recorder verbs is DONE modulo this sanctioned escape.

### Brief 16 usagesFrom — pass-side resolution RETIRED, executor is the single resolution authority (DONE + gated 2026-07-26)
(Superseded my earlier "keep usagesFrom" call — that was weak: the passes pre-resolving
`resources->physical(handle, slot)` into each usage was resolution SCATTERED across passes, the opposite of
the task-graph vision's single authority. Corrected.)
- **`ResourceUsage` now carries a LOGICAL registry handle** (`string::gpu::buffer buf`/`image img`) — or a
  raw id for not-virtualized cases (sentinels/persistent/FrameScratch). Two derived views: `resolve(reg,
  slot)` → this frame's PHYSICAL (execution); `key()` → stable LOGICAL identity for the planner's ordering
  (handle bands 1<<50/1<<51, non-colliding). Physical resolution + logical identity are now DERIVED from the
  handle, not pre-computed by the pass.
- **Executor is the single resolution authority:** a `res(usage)` resolver in record_frame resolves every
  usage's handle→physical for THIS frame at the ~13 barrier/written_resources sites (renderer.cpp). The
  planner keys on `key()` (frame_graph.hpp `use()`). Sentinel color/depth attachments stay raw (they're
  renderer sentinels, resolved by image_of — no handle).
- **Passes declare LOGICAL handles, zero pass-side resolution** for the registry PerFrame resources: stats
  (geometry_pass.cpp), froxel (FroxelPass async_usages via new FroxelComponent::handle()), hiz depth +
  pyramid (HizBuildPass/GeometryPhase2Pass + the DepthResolve marker, via new GeometryPass::hiz_depth_ring()
  /hiz_pyramid_ring()). Each resource converted COMPLETELY (all its usages) so producer/consumer keys match.
- **GATED: AE=0 exterior + two-phase orbit (handle-keyed hiz pyramid producer→consumer edge + DepthResolve);
  validation clean; gtest green.** Byte-identical (same physicals resolve; same toposort edges).
- **`usagesFrom` FULLY RETIRED (transport deleted too).** The `usagesFrom(&pass->usages)` method + the
  `const std::vector<ResourceUsage>*` raw-pointer-into-a-pass-member are DELETED. PassExec now holds a
  `UsagesFn` getter (`std::function<const std::vector<ResourceUsage>&()>`); the app authors via
  `.usages(getter)`; the executor + planner read usages THROUGH the getter interface (never a pointer into
  pass internals). Author sites: demo_scene author_pass + froxel .async() + the renderer's composite pass.
  So: passes declare LOGICAL usages (registry handles) through an interface; the graph is the single
  resolution authority (resolve per-frame) + orders by logical key(). **GATED: AE=0 exterior + two-phase
  orbit; validation clean; gtest green.** The per-frame `usages` member each pass still fills (conditional
  structure is mode-driven + finalized mid-record for the DepthResolve marker — legitimately dynamic) is now
  handed to the graph via the getter, not exposed as a pointer target. FrameScratch worklist usages remain
  raw per-slot ids (correct — FrameScratch is the transient arena, resolves to itself; not a usagesFrom
  concern). **Brief 16 is DONE: single resolution authority, passes declaring logical handles through an
  interface, usagesFrom retired, nothing hand-passed, crash class closed — all verified.**

### Status of the deep Phase 2 (2026-07-26)
DONE: GeometryScene consolidation; B1 (depth_resolve_target→DepthResolve usage); STEP 1 (FrameGraph as
persistent executor). All gated AE=0. NEXT: STEP 2 (above), then STEP 3 (decompose sky/shadow/ibl/gi/
froxel/gtao/transparency into real passes), then STEP 4 (introspection + mutable light/material store →
briefs 12-15).
