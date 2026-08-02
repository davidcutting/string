# Brief 04d — Two-pass occlusion culling + portable cull module

Status: COMPLETE (2026-07-22). M1 DONE, M2 CLOSED (re-baselined — two-phase is the new reference),
M3 COMPLETE (hiz-build prepass collapsed on every camera; dead prepass removed). See the M1/M2/M3
close-out sections below. NEEDS VISUAL VERIFY checklist at the end of Acceptance.
REVISED 2026-07-22 post-04c-resolution-(b): the design
below is the per-draw-command shape (the original entry-worklist/culling-in-
compute design was invalidated by the raster-order determinism finding — see
04c's Status). BLOCKED on brief 06's tooling surfaces landing (in flight);
calibrate milestone-4 targets from the 04c closing Tracy tables before
spawning.

## Goal

Eliminate the double geometry rasterization (the measured dominant cost:
the depth prepass re-rasterizes the full opaque set every frame) with
**two-phase temporal occlusion culling** — render last-frame-visible meshlets
first, build HiZ from that, test and render the rest the same frame.
Alongside it, extract per-meshlet culling into a **shared Slang module** so
the task shader and any future backend share one implementation.

## The determinism rule (governs this whole brief)

From 04c, proven on hardware and locked: **per-meshlet decisions must never
determine raster submission order.** Vulkan guarantees rasterization order
between COMMANDS, never between workgroups of one dispatch — coplanar depth
ties re-roll their winner otherwise (the floor shimmer the user saw).
Therefore:
- Submission structure stays per-draw: order-stable scan-compacted
  `VkDrawMeshTasksIndirectCommandEXT` per draw, consumed via
  `vkCmdDrawMeshTasksIndirectCountEXT` (the 04c machinery, unchanged).
- Per-MESHLET tests (frustum/cone/HiZ/phase membership) run in the TASK
  shader, inside a draw's commands — their results never cross draws.
- Per-DRAW decisions (survival, LOD, phase filtering) may run in compute and
  reorder/compact freely — command boundaries carry the ordering.
- The portability seam survives at per-draw granularity: a future
  non-mesh-shader backend consumes the same compacted per-draw commands as
  multi-draw-indirect (also command-ordered, so it inherits determinism).

## Context (measured)

- Pre-04c interior: hiz-build zone 4.84 ms (includes rasterizing the FULL
  frustum-surviving opaque set depth-only) + main-draw 3.81 ms — everything
  opaque rasterizes twice. Post-04c tables live in the 04c brief close-out;
  RECALIBRATE the M4 targets from those numbers before spawning.
- Brief 03 had a two-pass visibility bitfield once; it died of implementation
  bugs (temporal popping), not concept. The standard architecture (AC:Unity
  2015 → Nanite 2021) is pop-free by construction: phase 2 catches
  disocclusions the SAME frame.
- Chunking was finalized OFF in 04c — no chunking language applies.

## Decisions (locked with user, 2026-07-22 discussions)

- **Shared cull module `sandbox/shaders/meshlet_cull.slang`**: pure,
  binding-free functions, everything passed as arguments (bindless slots are
  plain uints): `sphere_in_frustum(planes, c, r)`, `cone_backfacing(axis,
  cutoff, c, r, eye)`, `hiz_visible(slot, dims, mips, cull_vp, c, r)`,
  `select_lod(lods, c, eye, focal, error_px)`. Currently these are inlined
  (duplicated) across meshlet_mesh / meshlet_shadow / meshlet_draw_cull —
  the refactor removes a proven multi-file-bugfix hazard. Orchestration
  (ballots, payload packing, compaction) stays in the consumers, NOT the
  module.
- **Two-phase visibility, per-draw command shape:**
  - Persistent per-meshlet **visibility bitfield**, indexed by the draw's
    global meshlet index AT ITS CURRENTLY SELECTED LOD. An LOD switch makes
    that draw's meshlets "new" → they take phase 2 that frame (conservative;
    do NOT remap bits across LODs). The draw phase already knows each draw's
    selected LOD — store last frame's LOD per draw to detect the switch.
  - **Phase 1**: compacted per-draw commands (04c path); the task shader
    tests frustum/cone (module) + bitfield — renders meshlets visible last
    frame DIRECTLY into the main color+depth targets. No depth-only prepass
    exists anymore.
  - **HiZ pyramid** from phase-1 depth. MSAA wrinkle: phase 1 renders into
    the MSAA targets, so the pyramid sources from a min-resolve of MSAA
    depth (small resolve pass), then the existing conservative
    uniform-stretch mip0 + 2x2 min chain.
  - **Phase 2**: a SECOND compacted command list; the task shader tests the
    complement (frustum/cone survivors whose bit is clear) against the fresh
    pyramid → renders survivors into the same targets → sets their bits.
    Clears bits for meshlets that failed this frame.
  - **Draw-level phase filtering (the compute's remaining role, and it is
    deterministic)**: the draw phase counts each draw's phase-2 candidates
    (clear-bit meshlets); compaction SKIPS draws with zero candidates from
    the phase-2 command list entirely. A fully-visible-last-frame scene
    issues a near-empty phase 2.
  - Bitfield lifecycle: sized to total meshlet capacity; cleared per-draw on
    streaming/residency changes (residency callback seam) and on LOD switch.
    A cleared bitfield is CORRECT, not special: everything takes phase 2 for
    one frame (warmup = fallback = same path).
  - **Freeze (F)** freezes the cull inputs AND the bitfield updates (frozen
    frustum + evolving bits would lie to the debug eye). Document the freeze
    semantics in the code.
- **Shadow cascades unchanged this brief** (their measured cost never
  justified churn). Transparency unchanged (tests against the FINAL opaque
  pyramid as today). Sky/froxels/shading untouched.
- **Debug**: `r.hiz.enabled` (STRING_HIZ) now toggles the two-phase test
  entirely (off = everything renders phase-1-style unconditionally — the
  A/B lever agents rely on). New phase-2 tint debug view (`r.debug.view`
  slot): phase-2-rendered meshlets highlighted — the disocclusion set should
  be SMALL and hug screen edges/occluder silhouettes during motion; a
  full-screen tint means the bitfield is broken. This view is also the
  tripwire for the old known HiZ timing anomaly (see memory/04c notes) if it
  survives the rewrite.
- **Determinism gates unchanged in spirit**: settled-frame captures AE=0
  run-to-run AND vs pre-brief baselines once visibility converges (the
  phase-1 ∪ phase-2 rendered set equals today's visible set; same per-draw
  command ordering ⇒ same image).

## Milestones

1. **Cull module refactor**: meshlet_cull.slang extracted; all inlined call
   sites (mesh, shadow, draw-cull) import it. Pure refactor gates:
   interior/exterior/crowd captures AE=0; shadow parity AE=0; hot reload of
   an edited module function propagates (import-closure cache covers it —
   verify once).
2. **Two-phase visibility** (the payoff): bitfield + phase-1/phase-2 command
   lists + draw-level phase filtering + MSAA min-resolve + pyramid rewire;
   depth-only prepass DELETED (03c discipline: grep gate + render proof).
   Gates: settled captures AE=0 vs pre-brief baselines; run-twice AE=0 x3;
   phase-2 debug view sane under motion (captures at consecutive frames
   while the camera moves — the phase-2 set must be small and edge-hugging);
   r.hiz.enabled off/on behaves; freeze semantics verified; validation +
   sync-validation clean.
3. **Perf close-out**: per-pass GPU tables (tools/tracy_pass_table.sh) on
   interior/exterior/crowd vs the 04c closing tables. Target (calibrate
   exact numbers pre-spawn): the prepass share of the old hiz-build zone
   collapses to min-resolve + pyramid + phase-2 raster; interior total
   materially down; no camera regresses. Report honestly with root cause if
   missed. Update this brief + the 06 baseline section.

## M1 close-out — cull module extraction (2026-07-22)

`sandbox/shaders/meshlet_cull.slang` created: pure, binding-free `frustum_planes`,
`sphere_in_frustum`, `cone_backfacing`, `hiz_visible(tex[], slot, dims, mips, cull_vp, eye, c, r)`,
`select_lod(lods, lod_count, c, eye, focal, error_px)` — verbatim extraction of the math that was
inlined (duplicated) in meshlet_mesh / meshlet_shadow / meshlet_draw_cull. All three now `import
meshlet_cull`; the inlined copies are DELETED. Orchestration (ballots, payload packing, worklist
compaction, stats atomics) stays in the consumers.

Gates (all PASS):
- Pure-refactor parity AE=0 vs pre-M1 baselines: interior (HiZ on AND off), exterior, crowd all
  `magick compare -metric AE == 0`. [mesh-cull] stats bit-identical (interior 62267->40807->39218
  ->22673 LOD 431/17/0/2; exterior 52707->52707->51668->11226; crowd 554535->...->29831).
- Shadow parity AE=0 — the interior capture exercises the (rewritten) shadow task shader.
- Hot-reload / import-closure cache verified: a temporary edit to `select_lod` (force coarsest LOD)
  propagated on relaunch (LOD histogram 431/17/0/2 -> 145/7/14/284, meshlets 62267 -> 18156),
  proving the compiler's import-closure hash (globs every .slang in the search dir) invalidates the
  stale cached SPIR-V. Reverted clean, post-revert AE=0. NOTE: LIVE (running-process) hot-reload of a
  module edit does not auto-swap because the file-watcher only tracks REGISTERED program sources, not
  imported modules — this is pre-existing for ALL modules (meshlet/lighting/sky.slang) and out of
  scope; the closure cache covers the correctness-on-next-compile requirement the gate asked for.

## M2 progress — renderer foundation landed (2026-07-22, in progress)

The renderer's single-scene-render-pass ownership forbids dispatching the pyramid-build compute
between two sets of opaque draws into the SAME MSAA targets. Resolved with a minimal, additive,
flag-guarded renderer capability (verified byte-neutral, AE=0, while dormant):
- `Pass::breaks_scene_group()` + `Pass::record_between()` (render_pass.hpp): a pass can force the
  renderer to END the scene render group after it, run a COMPUTE step OUTSIDE rendering
  (`record_between`), then reopen the next scene group.
- renderer.cpp scene-group loop: consecutive COLOR_TARGET (MSAA) groups now chain — the FIRST clears
  msaa_color_/msaa_depth_, later groups LOAD (preserve) them, and only the LAST resolves
  msaa_color_ -> color_attachment_ (intermediate MSAA groups STORE their samples + depth). A
  non-first MSAA group does not discard the loaded depth.
- `msaa_depth_` gains `VK_IMAGE_USAGE_SAMPLED_BIT` (init + resize) so the min-resolve compute can
  read its MSAA samples for the pyramid mip-0.
Existing single-group passes are unaffected (with no breaker, the one scene group is first+last ->
clear+resolve exactly as before; interior capture AE=0 vs pre-brief baseline).

### M2 IMPLEMENTED (2026-07-22) — functional, deterministic, validation-clean; settled-parity gap open

The full two-phase path is built and RENDERS CORRECTLY with working occlusion:
- **Depth min-resolve chosen over an MS-sampling compute shader**: captures run on real RADV/RDNA3
  (not the software raster used for the 04c perf tables), which supports `VK_RESOLVE_MODE_MIN_BIT`.
  The renderer MIN-resolves the phase-1 MSAA depth into the geometry pass's single-sample `hz.depth`
  at phase-1's EndRendering (reverse-Z: min = farthest = conservative occluder), so no MS-sampling
  descriptor changes and no new compute shader are needed. `record_between()` then feeds `hz.depth`
  into the UNCHANGED hiz_build.slang uniform-stretch mip0 + 2x2 min chain.
- **Persistent per-meshlet bitfield** (`visbits_buffer_`, ceil(total_meshlets/32) words, device-local,
  zero-init via vkCmdFillBuffer). `VisBitfield` in meshlet.slang with atomic vis_get/set/clear.
  Cleared on first frame + on any residency change (streaming seam wired in the residency callback).
- **Phase-split task shader** (meshlet_mesh.slang, MeshletPush grows to 256B, std430 offsets verified
  via slangc): phase 1 renders bit-SET meshlets (no HiZ); phase 2 tests EVERY frustum+cone survivor
  against the fresh pyramid to keep its bit honest, but only DRAWS the newly-visible bit-CLEAR
  complement (bit-set meshlets were already drawn in phase 1 — re-drawing would double-raster + break
  determinism). New debug view 4 tints the phase-2 disocclusion set cyan; view 3 shows phase-2 HiZ
  rejects red.
- **geometry_pass restructured**: record() draws sky + phase-1; renderer breaks the scene group,
  MIN-resolves depth, calls record_between() (pyramid), then record_after_between() draws phase-2 +
  transparency into the reloaded MSAA targets. The depth-only prepass render + its pyramid build in
  record_compute are DELETED; the prepass worklists/buffers remain allocated (dead — to be removed in
  the grep-gate cleanup).

**Gates status:**
- HiZ-OFF (single-pass phase-0, no group break): AE=0 vs pre-brief HiZ-off baseline — the renderer
  changes + phase-0 path are byte-neutral.
- Two-phase (HiZ-on) run-twice: AE=0 (deterministic; the 04c coplanar-stability fix HOLDS).
- Validation: CLEAN (only the benign dzn-ICD-skip line).
- Occlusion converges: settled after_hiz 24276 (phase2=0), down from the cold-start 39218 == all
  cone survivors — phase 2 clears occluded bits correctly.
- **OPEN — settled parity vs baseline is NOT AE=0**: ~1.6% RMSE / 0.385 raw AE (0.03 at 3% fuzz — a
  low-amplitude broad difference, NOT structural). ROOT CAUSE (diagnosed): the two-phase pyramid is
  built from phase-1 at the CAMERA-selected LOD, whereas the pre-brief single-pass built its pyramid
  from a FORCED-LOD0 depth prepass (fuller silhouette). The camera-LOD pyramid is slightly less
  conservative, so two-phase draws ~1603 extra thin silhouette-edge meshlets (24276 vs 22673) — the
  same "thin ceiling-beam slivers past the curtain silhouette" class brief 03 documented its baseline
  HiZ as OVER-culling. Two-phase draws them (arguably MORE correct), but that means the drawn set no
  longer byte-matches the baseline. Reaching AE=0 needs the two-phase pyramid to match the baseline's
  LOD0 conservatism (e.g. phase-1 depth at LOD0, or a fuller-occluder pyramid pass) — a design
  reconciliation still open.

Remaining M2/M3: close the settled-parity gap (or agree with the user that the two-phase set is the
new correct reference and re-baseline); draw-level phase-2 candidate skip (perf); phase-2 debug-view-
under-motion + freeze visual checks; grep gate + dead-prepass-buffer deletion; perf tables.

### M2 CLOSED (2026-07-22) — RE-BASELINED, resolution (b)

The settled-parity gap is RESOLVED by re-baselining: the two-phase output is the new correct
reference. Rationale (one paragraph, per the settled decision): the OLD baseline's HiZ pyramid came
from a forced-LOD0 depth prepass whose fuller silhouettes OVER-culled thin visible geometry — the
documented "accepted 1.27% interior HiZ residual" from brief 03. Direct visual evidence confirms it:
the two-phase ceiling crop shows the hanging IVY VINES and thin RIGGING lines PRESENT that are MISSING
in the pre-brief baseline (over-culled), with the beam/floor body byte-identical between them (no
structural geometry removed). The two-phase image is MORE correct. **Over-cull recovery number (the
correctness headline): the interior HiZ-on-vs-HiZ-off residual went from AE 9048 / RMSE 1.30% (old
forced-LOD0 pyramid, over-culling visible slivers) to AE 241998 / RMSE 1.08% (two-phase pyramid). RMSE
dropped, but AE rose because the two-phase pyramid now DRAWS the recovered slivers (many more pixels
differ from the naive HiZ-off image, but each by a small amount — the difference is thin edge geometry
appearing, not disappearing).** Both the residual and the vs-old-baseline delta collapse under a fuzz
sweep (interior HiZ residual: AE 241998@0% → 1518@5% → 85@10%), proving a broad low-amplitude edge
difference, NOT a structural block — corroborated by the >5% structural-diff mask showing only thin
ceiling/silhouette slivers, floor and walls clean.

Gates (all PASS on real RADV/RDNA3 unless noted):
- Determinism at the new reference: interior / exterior / crowd run-twice x3 each — ALL AE=0.
- HiZ-OFF (new) vs OLD pre-brief hizoff baseline: AE=0 (renderer changes byte-neutral, re-verified).
- Delta vs OLD baselines (added-geometry class, NO removed real geometry — signed both-direction check
  + >5% mask + ceiling crop): interior AE 246553 (0.385), exterior AE 47408 (0.074), crowd AE 437385
  (0.683). All thin silhouette/ivy/rigging slivers recovered; no structural loss in any camera.
- Warmup (frame 10, cold bitfield → everything via phase 2): FULL scene present, not black/partial.
- Phase-2 debug view (r.debug.view / STRING_VIEW 4) at settled static frame: ZERO cyan (phase2_drawn=0
  — converged), i.e. NOT full-screen-broken. Motion view-4 not verifiable headlessly (see below).
- Validation CLEAN (standard). **Sync-validation: two-phase-specific hazards FIXED this session** —
  the MSAA MIN-depth-resolve barriers were missing the resolve's true stage/access (resolves execute
  in COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE, not the depth stages) and the msaa_depth_
  store->reload transition's tracker src stage was too narrow (EARLY only, needed EARLY|LATE). After
  the fixes (renderer.cpp depth-resolve transition, geometry_pass.cpp record_between + depth_target
  usage stage) sync-validation reports ZERO two-phase hazards; the only remaining line is the
  pre-existing vkAcquireNextImageKHR WAR present identically in the HiZ-off baseline path. All fixes
  byte-neutral (AE=0). Layer enabled via VK_LAYER_VALIDATE_SYNC=1.
- r.hiz.enabled on/off lever works (HiZ-off = single-group phase-0, AE=0 vs baseline). STRING_TRANSP_TEST
  still composites correctly against the final opaque pyramid.
- Freeze (F): key-only (no CVar), so not directly drivable headlessly. Static-camera captures are the
  frozen-input equivalent and are stable frame-to-frame (settled AE=0 run-to-run). REMAINS for the user
  flythrough: press-F visual check that the frozen frustum + frozen bitfield hold (code path exists;
  push.freeze_bits gates the bitfield update). NOTE: STRING_CAPTURE_EVERY_N is not honoured headlessly
  because capture_every_n_cvar() isn't registered before the renderer's apply_env() (pre-existing env
  ordering limitation, out of scope) — verified via the source, not a regression.
- Motion sanity for view-4: the demo (Sponza) scene has NO scripted-motion lever (only the ui-dev
  scene orbits, and it renders no geometry); STRING_CAM is a static placement. So the "small,
  edge-hugging cyan set under motion" check REMAINS a user-flythrough item; the static+debug-view
  evidence (phase2_drawn=0 converged, warmup full via phase 2) stands in its place headlessly.

### M3 COMPLETE (2026-07-22) — perf close-out + dead-prepass removal

Dead-code cleanup (grep gate: 0 dangling camera-prepass refs). Deleted: wl_prepass_opaque_/
wl_prepass_twosided_ (worklists), prepass_lod_buffer_ (+ its memset), the `bool prepass` parameter of
record_draw_cull and all its branches (collapsed to the camera path; the shadow `cascade>=0` path is
untouched). `force_lod0` (push field + shader read) is now always 0 — RETAINED as a padding slot
(offset 156, static_assert intact) to avoid a push-layout renumber for zero benefit; documented DEAD.
Stale comments on the HizPyramid depth image (now the two-phase MIN-resolve target, not a prepass
render) corrected. Net: ~35 LOC removed. Post-cleanup parity AE=0 on all three cameras + hizoff; flake
check (engine + cook unit tests) green; standard validation clean.

Per-pass GPU tables (.#demo-tracy, software-raster RELATIVE ms, same method as 04c: tracy-capture +
tracy-csvexport -g, mean of "GPU execution time", STRING_LIGHTS=0, settled window). Tracy zone map:
`main-draw` = phase-1 opaque draws; `hiz-build` = the between step (MSAA MIN depth-resolve + pyramid);
`phase2` = the disocclusion complement pass. (`geometry` is the renderer's container scope wrapping the
whole group — overlapping, not additive.) `[mesh-cull]` stats semantics under two-phase:
meshlets_total/after_frustum/after_cone DOUBLE-count (task shader runs once per phase per meshlet);
after_hiz = phase1∪phase2 DRAWN (== the visible set); phase2 = disocclusion complement drawn this frame
(settled interior = 0, i.e. fully converged, no popping).

| Stage           | 04c int(c=0) | 04d int | 04c ext | 04d ext | 04c crowd | 04d crowd |
|-----------------|-------------:|--------:|--------:|--------:|----------:|----------:|
| draw-cull       |   0.024      |  0.016  |  0.036  |  0.032  |   0.589   |   0.225   |
| expand          |   0.052      |  0.029  |  0.066  |  0.071  |   0.139   |   0.062   |
| hiz-build       |   5.13       |  0.121  |  7.37   |  0.165  |  82.98    |   0.126   |
| main-draw       |   4.45       |  4.027  |  2.20   |  3.359  |   9.38    |   5.751   |
| phase2 (new)    |   —          |  0.445  |   —     |  0.786  |    —      |   5.738   |
| shadow-cascades |   0.56       |  0.447  |  1.44   |  1.140  |  27.05    |  13.795   |
| transparency    |   0.154      |  0.202  |  0.023  |  0.035  |   0.164   |   0.151   |
| froxel-cull     |   0.119      |  0.121  |  0.116  |  0.127  |   0.112   |   0.118   |
| sky             |   0.114      |  0.110  |  0.131  |  0.144  |   0.107   |   0.110   |

Verdict (honest, per the brief's targets):
- **The prepass share of hiz-build collapses** on EVERY camera: interior 5.13→0.121, exterior
  7.37→0.165, crowd 82.98→0.126 ms. The old zone was the full opaque-set depth rasterization; it is
  entirely gone. The recovered visibility work reappears as `phase2` — a fraction of it.
- **Raster+HiZ total (hiz-build + main-draw + phase2)**: interior 9.58→4.59 (−52%), exterior
  9.57→4.31 (−55%), crowd 92.36→11.62 (−87%). The ivy double-raster was the interior prize; the
  crowd's pathological 83 ms prepass was the biggest absolute win.
- **No camera regresses** — every camera's raster+HiZ total is materially down; shadow-cascades also
  fell (crowd 27.05→13.80, a side benefit of the shared cull module + compaction, not a regression).
  main-draw rose slightly on exterior (2.20→3.36) because phase-1 now renders color (not depth-only)
  for the last-frame-visible set — but that is MORE than offset by hiz-build's 7.2 ms collapse.
- No miss to root-cause: all three cameras improved on the headline metric.

RADV/RDNA3 (hardware) sanity, interior settled: geometry(phase-1)=3.78, geometry(between/hiz-build)=
0.11, geometry(phase2)=0.40 ms — same shape as the software table, confirming the pyramid build is
now trivial and the phase-2 raster is small.

## Post-close motion-ghosting fix (2026-07-22)

**The bug (user-visible):** under CAMERA MOTION in Sponza, large surfaces (the user's screenshot:
the interior ceiling vault) rendered SEMI-TRANSPARENT — arches behind showed through at ~half
opacity, with a ghost overlay resembling geometry from a slightly-different (previous-frame) camera
angle. Everything looked "kinda transparent." STATIC/settled frames were PERFECT (all the AE=0 gates
above held) because on a converged static frame phase 2 draws NOTHING (phase2_drawn=0) and the
multi-group MSAA chain degenerates to a single effective writer — so no headless gate (all static)
ever exercised the phase-1/phase-2 interleaved multi-group path where the bug lives.

**Root cause (precise mechanism):** the two-phase MSAA color image `msaa_color_` is NOT a graph
resource (passes declare the RESOLVED single-sample `color_attachment_` as their ColorWrite usage,
not the multisample target). The renderer's group loop therefore never emits a barrier for
`msaa_color_` between MSAA groups — for a non-first MSAA group its layout is already
COLOR_ATTACHMENT_OPTIMAL from the previous group's STORE, so the sole `resource_states_.transition`
call (guarded by `msaa_is_first`) is skipped entirely and no transition/barrier runs. Consequently
there was NO execution+memory dependency between the phase-1 group's color STORE
(COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE) and the phase-2 group's LOAD_OP_LOAD + its
AVERAGE-resolve READ of the same image. On a static frame phase 2 wrote no samples, so the resolve
happened to read intact phase-1 samples and the hazard never bit. Under motion, phase 2 writes the
disocclusion complement INTO the shared MSAA image while phase-1's stores are neither guaranteed
available nor visible, and the AVERAGE resolve then mixes stale (previous-content / partially-written)
and fresh samples per pixel — averaging stale+fresh coverage is exactly a semi-transparent, ghosted
image. The analogous depth STORE->LOAD dependency WAS already covered only because `msaa_depth_` IS a
graph resource (its DepthWrite usage drives a real tracker barrier); `msaa_color_` had no such seam.
(Note: sync-validation did NOT flag this — the layer's coverage of dynamic-rendering multisample
attachment store/load self-hazards on a non-tracked image missed it; the bug was found by the capture
gate, not the validator.)

**The fix (minimal, in `renderer.cpp` group loop):** for a non-first MSAA group (`msaa_group &&
!msaa_is_first`), emit an explicit `vkCmdPipelineBarrier2` image self-barrier on `msaa_color_`
(COLOR_ATTACHMENT_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL) with src = COLOR_ATTACHMENT_OUTPUT /
COLOR_ATTACHMENT_WRITE and dst = COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE|READ, right where
the first-group branch would otherwise transition it. This makes the previous group's stored samples
both available and visible to the next group's load + resolve. Byte-neutral on static frames (phase 2
empty -> identical resolve output).

**Tooling gap-fixes (permanent), needed to reach the bug headlessly:**
- `dbg.orbit` CVar (float, default 0 = off; alias STRING_ORBIT). Nonzero = the demo camera sways
  continuously at that angular speed (rad/s) around a latched base pose (small radius + coupled
  yaw/pitch sweep) — enough per-frame disocclusion to make phase 2 non-empty. Implemented in
  `GeometryPass::update` (sandbox), applied AFTER `camera_.update`. Added `Camera::yaw()/pitch()`
  getters so the sway reads the base angles. This is the permanent headless MOTION lever the brief's
  close-out flagged as missing.
- `capture_every_n_cvar()` is now touched BEFORE `apply_env()` in the renderer ctor, so
  STRING_CAPTURE_EVERY_N is honoured headlessly (previously the accessor was first touched at frame
  end, long after apply_env, so its env override never registered — the "pre-existing env ordering
  limitation" the M2 close-out noted as out of scope). Headless capture SEQUENCES now work from env.
- `tools/capture_seq.sh`: capture-sequence helper (mirrors capture.sh's resource stitching) driving
  STRING_ORBIT + STRING_CAPTURE_EVERY_N to write a numbered PNG sequence.

**Gates (all PASS, real RADV/RDNA3):**
- Motion sequence (STRING_ORBIT=0.5, every_n=5, interior, STRING_LIGHTS=0): READ the captures — the
  ceiling vault is OPAQUE in EVERY orbit frame (verified frames 60/90/115/120/160, all with nonzero
  phase-2 under continuous orbit; frame 120 logged phase2=28). Ghosting GONE. Pre-fix the same frames
  showed the semi-transparent ceiling; HiZ-off (single-phase) was ALWAYS clean, isolating the bug to
  the two-phase multi-group machinery. View-4 under motion: phase-2 cyan set small + edge-hugging.
- Static parity re-verified byte-neutral: interior HiZ-on AE=0 vs the two-phase reference
  (m2c_interior); interior HiZ-off AE=0 vs the old hizoff baseline; interior/exterior/crowd run-twice
  AE=0 (determinism holds).
- Warmup (frame 10): full scene present (not black/partial).
- Validation CLEAN under motion; sync-validation (VK_LAYER_VALIDATE_SYNC=1) under motion CLEAN of
  two-phase hazards (only the pre-existing vkAcquireNextImageKHR WAR line remains). No new hazard from
  the added barrier.
- nix flake check green; .#demo + .#demo-tracy build green. STRING_TRANSP_TEST still composites the
  staggered blended quads correctly (transparency path untouched). r.hiz on/off lever unaffected.

## Acceptance

- All parity/determinism gates; flake check + .#demo + .#demo-tracy green;
  validation clean; cooked assets untouched (runtime-only brief); all CVar
  levers + F/C/O/L behavior preserved.
- NEEDS VISUAL VERIFY (consolidated, priority order — headless evidence covered
  determinism/warmup/static-view-4/parity, but these need a live flythrough):
  1. **Coplanar/floor stability flythrough** — the 04c coplanar-shimmer fix must
     HOLD under two-phase. Fly low over the floor/coplanar surfaces; must stay
     rock-stable (headless run-twice AE=0 is necessary but not sufficient — motion
     is the real test).
  2. **Disocclusion popping hunt** — strafe past pillars/curtains and reveal
     occluded areas quickly; watch for one-frame holes / late-appearing geometry.
     Phase-2 catches disocclusions the SAME frame by design, so this should be
     pop-free; confirm.
  3. **The RECOVERED geometry** — hanging IVY vines and thin RIGGING/ceiling-beam
     slivers are now visible that the old baseline over-culled (confirmed in the
     static ceiling crop). Expect slightly MORE correct detail than the old build,
     NOT less. If anything looks MISSING vs the old build, flag it (the analysis
     found only additions, no structural loss).
  4. **Phase-2 debug view (r.debug.view 4 / STRING_VIEW=4) under MOTION** — the
     cyan disocclusion set should be SMALL and hug screen edges / occluder
     silhouettes while moving (it is empty at rest — converged). A full-screen
     cyan tint means the bitfield is broken. (Not verifiable headlessly — no
     scripted demo-scene motion lever.)
  5. **Freeze (F)** — press F while moving: the frozen frustum + frozen bitfield
     should hold (bitfield updates gate on push.freeze_bits); the frozen cull set
     should not evolve. LOD-transition correctness while flying (an LOD switch
     makes a draw's meshlets take phase 2 for one frame — should be seamless).

## Two-phase transparency + over-cull fixes (2026-07-23, USER-VERIFIED FIXED)

A user on a ~1600x1600 window reported severe artifacts with HiZ/two-phase on:
PERSISTENT semi-transparent surfaces (the roofline visible THROUGH the ceiling,
static camera included) plus fragment popping under motion. O (HiZ toggle) cured
it. Every prior agent gate ran headless at 800x800 and judged captures "clean",
so the break shipped invisibly. FOUR root causes were found; #4 was THE
transparency/popping bug the user saw (their fix confirmation), #1-#3 are real
latent defects fixed along the way:

1. **HiZ pyramid coarse-mip sparse sampling (resolution-dependent STATIC
   over-cull).** `hiz_build.slang`'s pyramid->pyramid reduce hardcoded
   `SampleLevel(uv, 0)` — it always sampled MIP 0 while using the *current* (coarser)
   level's grid. So every coarse mip min-reduced only a SPARSE subset of mip-0 texels
   and MISSED the farthest ones between samples; coarse `occ` came back too NEAR and
   phase-2 wrongly rejected visible meshlets. The error compounds with mip count, so
   11-mip (>=1024 base, i.e. screens >800px) pyramids over-culled hard while the 10-mip
   512 pyramid at 800x800 barely showed it. Fix: added `src_level` to the HiZ push and
   sample `SampleLevel(uv, src_level)` (the level actually being reduced). Proven with a
   forced-coarsest-mip debug: pre-fix the whole scene was HiZ-rejected (near garbage in
   the top mips); post-fix the coarsest mip = true whole-screen farthest and everything
   passes. LATENT SINCE BRIEF 03 (the inline HiZ build had the same bug; 04d's shared
   `meshlet_cull.slang::hiz_visible` is verbatim-correct and was NOT the cause).

2. **Cross-frame race on the persistent visibility bitfield (STATIC flicker).**
   `visbits_buffer_` is a single, intentionally-non-ringed buffer (temporal state
   accumulates). With frames-in-flight=3 and no frame-boundary barrier, frame N's
   phase-1 TASK-shader READ raced frame N-1's phase-2 TASK-shader WRITE, flipping a
   meshlet's bit between consecutive frames -> its phase-2 draw appeared/disappeared
   even on a STATIC camera (measured 23.5% -> AE=0 frame-to-frame after the fix). Fix:
   a TASK->TASK `SHADER_STORAGE_WRITE -> READ|WRITE` barrier at the top of
   `GeometryPass::record_compute` (OUTSIDE dynamic rendering, where TASK is a legal
   stage; same-queue submission order makes its src scope cover the prior frame's
   phase-2 writes).

3. **Cross-frame WAR on the shared MSAA color image (latent race, hardening).**
   `msaa_color_` is a SINGLE shared image, not ringed. Frame N's phase-1
   `LOAD_OP_CLEAR` + writes could race frame N-1's still-in-flight AVERAGE resolve READ:
   the "first MSAA group" path only called `resource_states_.transition()`, a no-op
   across the frame boundary (tracker already holds COLOR_ATTACHMENT_OPTIMAL), and the
   motion-ghosting self-barrier fires only INTRA-frame (`!msaa_is_first`). Fix: an
   unconditional `COLOR_ATTACHMENT_OUTPUT` self-barrier on `msaa_color_` in the
   `msaa_is_first` branch (src WRITE|READ -> dst WRITE), before `vkCmdBeginRendering`.
   (Initially blamed for the transparency; disproven by `STRING_SERIALIZE_FRAMES=1`
   showing identical artifacts — but the hazard is real and stays fixed.)

4. **PHASE-2 + TRANSPARENCY RENDERED WITH NO DEPTH ATTACHMENT (THE transparency
   bug — user-verified fix).** When two-phase is active, GeometryPass breaks its render
   group and phase-2 (`record_after_between`: disocclusion opaque + sorted transparency)
   is recorded at the START of the NEXT group, inheriting THAT group's attachments. The
   next group is DebugLinePass's, which declares depth as `Access::DepthRead` (tests,
   never writes) — and renderer.cpp's `depth_target_of` matched ONLY `Access::DepthWrite`
   -> `group_depth = nullopt` -> `pDepthAttachment = NULL` for the whole group. Phase-2
   therefore drew with NO DEPTH TEST: every meshlet it drew landed ON TOP of phase-1's
   correct image — far geometry over near, i.e. the roofline drawn over the ceiling.
   Symptom map: persistent transparency (phase-2 redraws its set every frame, static
   included); popping under motion (the phase-2 set churns); HiZ-off clean (no group
   break -> phase-2 doesn't exist and geometry's own DepthWrite binds depth);
   serialization-independent (logic bug, not a race). Fix (renderer.cpp): (a)
   `depth_target_of` falls back to `Access::DepthRead` so read-only groups still BIND the
   depth attachment (the debug-line pipeline was already built expecting one —
   `enable_depth_stencil(true, false)`); (b) when `group_depth` is bound, the group's
   depth transition is forced to the WRITE scope (EARLY|LATE fragment tests) so the
   tracked layout matches the attachment's DEPTH_STENCIL_ATTACHMENT_OPTIMAL and covers
   phase-2's depth writes.

**Debug levers added:** `STRING_FIXED_DT=<seconds>` (deterministic per-frame timestep so
dbg.orbit-driven A/B captures align by frame index) and `STRING_SERIALIZE_FRAMES=1`
(vkDeviceWaitIdle after every frame — if a live artifact survives it, it is NOT a
frames-in-flight race; this is what disproved the race theory and forced the audit that
found #4).

**Post-mortem — why agents kept missing it:** (a) 800x800-only gates: the sparse-mip
error (#1) scales with mip count and barely shows at a 512-base pyramid. (b) Headless
captures judged "clean" at a camera view with mostly SKY behind the surfaces — far-over-
near compositing there just looks like shading; the user's view had a roofline behind a
ceiling. Perceptual gates need a view with LAYERED geometry. (c) The two-phase group
split scattered responsibility: the bug lived in the RENDERER's group-forming lambda,
not in the geometry pass or shaders where all the two-phase logic (and hence all the
auditing) was concentrated. When a pass piggybacks on another group's render-pass
instance, the host group's attachment derivation is part of the feature's correctness
surface.
