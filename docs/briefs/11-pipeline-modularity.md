# Brief 11 — Render pipeline modularity + pass control

Status: **DRAFT / not started** (planning). Top-level render-graph **API shape locked with
user 2026-07-25**; milestones below are a first cut, refine before spawning. First brief of the
tooling/UI arc (11–15) inserted before 08 VFX — see `README.md` order + `docs/engine-feature-spec.md`.

## Goal

Two coupled outcomes:

1. **Decompose the `GeometryPass` god-object** (~2000 lines hosting shadow capture, sky, IBL,
   GI capture/relight/debug, froxel cull, main draw, transparency) into discrete, individually
   registered passes.
2. **Evolve the 04e render graph** from a per-frame-rebuilt plan into a **persistent,
   introspectable, fluent-authored graph** with first-class per-pass enable/disable — and
   *prove* modularity works by toggling passes live via CVars, not refactor-on-faith.

This is an **evolution of 04e, not a rewrite**: the derived-barrier core, the `ResourceUsage`
declaration model, aliasing, and the queue-lane machinery all stay. We are putting a clean
authoring facade over them and making the graph persistent + introspectable.

## Decisions locked with user (2026-07-25)

### Persistent, invalidatable plan (not per-frame rebuild)

- The graph **compiles once** — toposort, lifetime/aliasing analysis, and the *derived barrier
  set* (the expensive CPU work) are cached. Each frame **re-records** command buffers cheaply
  from the cached plan (one CB per frame-in-flight slot; a CB in flight can't be re-recorded, so
  N slots regardless).
- **Not literal CB resubmission**: swapchain image index + per-frame resources rotate, so the CB
  is re-recorded each frame — but the *plan* is reused. The win is skipping re-derivation, not
  reusing bytes.
- **Fits us unusually well because we are GPU-driven**: indirect-count draws + GPU cull mean
  nearly all per-frame variation lives in *buffers the GPU reads*, not in CB structure. CB
  structure is near-static frame-to-frame → re-record is trivial, and recorded secondaries can be
  cached where nothing structural changed.
- **Invalidation** (forces a recompile — rare, cheap): a `.toggle()` flip, resize, or pipeline
  hot-reload. Everything else reuses the plan.

### Fluent Daxa-style builder (the top-level API)

Virtual resources are handles; passes **declare** their I/O and the graph **derives** sync +
ordering. Canonical shape:

```cpp
// virtual resources — transient by default, sized by the graph; all are handles
auto hdr     = rg.image("hdr_color", {.format=RGBA16F, .size=viewport});
auto depth   = rg.image("depth",     {.format=D32, .samples=4, .size=viewport});
auto shadow  = rg.image("csm",       {.format=D32, .layers=3, .size={2048,2048}});
auto froxels = rg.buffer("froxel_lights", {.bytes=...});
auto swap    = rg.import(present_target);          // external/persistent resource

rg.pass("shadow.cascades")
    .writes(shadow, Depth)
    .toggle(cv_shadows)                              // r.shadow.enable — enable/disable
    .raster([&](RasterCtx& c){ c.draw_meshlets(shadow_list); });

rg.pass("froxel.cull")
    .reads(depth).writes(froxels)
    .queue(Queue::AsyncCompute)                      // lane HINT; scheduler decides placement
    .compute([&](ComputeCtx& c){ c.dispatch(groups); });

rg.pass("gi.relight")
    .reads(sh_atlas).read_writes(gi_irradiance)      // in-place atlas
    .run_if(sun_moved)                               // amortization as a run-condition
    .compute([&](ComputeCtx& c){ ... });

rg.pass("geometry.main")
    .reads(shadow, froxels, gi_irradiance)
    .color(hdr).depth(depth)
    .toggle(cv_geometry)
    .raster([&](RasterCtx& c){ c.draw_meshlets(main_list); });

rg.pass("composite").reads(hdr).color(swap).raster([&](RasterCtx& c){ c.fullscreen(); });

rg.compile();          // once — toposort, lifetimes, barrier derivation, aliasing (04e core)
rg.execute(frame);     // per frame — cheap re-record from the cached plan
```

- `.toggle(cvar)`: a disabled pass drops out of the toposort and barriers re-derive around it.
  The debug UI's pass checkbox (brief 14) writes the **same** CVar handle → toggling a pass from
  the panel is the live proof of modularity.
- `.queue()` is a hint; the scheduler is still the only placement decider (04e guardrail).
- `.run_if()` folds the IBL/GI amortization pattern into the graph as a per-pass run-condition.
- **Disabled-pass-consumer policy (RESOLVED 2026-07-25):** a pass runs as long as its *required*
  inputs exist — a single missing input does NOT skip the pass. Reads **degrade gracefully by
  default**: when a producer is toggled off, the reader binds a **neutral fallback** resource
  cleared to a value chosen so the term *cancels* — default zero/black (additive contributions
  vanish), with a **per-read override** where a different neutral is the identity (shadow → 1.0 =
  unshadowed, AO → 1.0, GI → the existing sky-SH fallback). The consumer still runs. Only reads
  explicitly marked **`.requires(x)`** (no meaningful neutral) transitively skip their consumer
  when the producer is off. This is what makes debug pass-toggling behave: shadows off → lit
  scene; geometry off → composite over cleared color + sky + UI, not a black screen. Precedent:
  the `r.gi 0` sky-SH fallback is already exactly this pattern, now formalized in the graph.

### Handles for GPU resources (lean — no generation system)

- GPU resources are referenced by lightweight `Handle<Image>` / `Handle<Buffer>` value types; the
  allocator/pool owns storage + lifetime. Handles are plain ids into the pool — **NO generation
  counter** (2026-07-25 user: don't add a generation system where it isn't needed. Timeline
  semaphores already guard the critical GPU-lifetime sync; use-after-free hardening is deferred,
  keep it lean).
- The graph's virtual resources **are** handles. The debug-UI target visualizer just enumerates
  live handles: `for (auto h : rg.images()) u.image(h)…`.

### Introspection + mutable state (the seam for briefs 14/15)

- Passes and targets are **enumerable with metadata** (name, format, purpose, lifetime).
- Light + material state is introspectable **and mutable** through a **handle-addressable store**
  (not raw pointers) — this is the write-back seam brief 15's gizmos/material-edit use and the
  read seam brief 14's inspectors use.

### Engine / sandbox split

Engine (`string::gpu`) owns the graph, handle pools, registry, and introspection API (all
generic). Sandbox owns the concrete passes and the scene-specific resource/state stores.

### Process + scope guardrails (2026-07-25)

- **Incremental, never big-bang.** Handles + persistent-plan conversion + GeometryPass
  decomposition land in stages (M0→M4) with a **working, byte-identical renderer at every
  milestone** (03c precedent). No mid-refactor state where the demo doesn't render.
- **Resource-management boundary for this brief**: bring the **main-pass color/depth targets +
  MSAA** into the graph (04e already flagged MSAA should be graph-tracked). **Leave the GI/IBL
  atlases + shadow maps as local-class** hand-managed for now (documented follow-up) — keeps the
  brief bounded.
- **Sequencing**: the tooling/UI arc runs **serially, brief 11 first** (not parallel) — shared
  `.#demo` build means concurrent agents break each other's baselines (04c/05 lesson).

## Milestones (first cut — refine before spawning)

- **M0** — Generational `Handle<Image>`/`Handle<Buffer>` pool (or promote what exists); stale-handle
  detection; allocator owns lifetime. Enumeration API.
- **M1** — Fluent builder facade over the existing 04e graph; **persistent compile + invalidate**
  (compile once, re-record per frame slot, invalidate on toggle/resize/reload). AE=0 vs current.
- **M2** — Decompose `GeometryPass` into individually registered passes. **Byte-identical with all
  passes enabled** (this IS a valid parity gate — pure refactor; 03c-deletion precedent).
- **M3** — Per-pass enable/disable via CVar `.toggle()` with the graceful-degrade fallback policy
  (neutral fallback resources + `.requires` transitive skip); toggling passes live behaves
  correctly (no validation errors, terms cancel as designed).
- **M4** — Introspection API (enumerate passes + targets w/ metadata) + handle-addressable
  light/material store seam. Consumed by briefs 14/15.

## Verification (per `README.md` rules — all apply)

- **Parity IS valid here** (unlike the CSM/GI case): with all passes enabled the decomposed graph
  must be byte-identical (AE=0) to the pre-brief output, at multiple resolutions incl. non-square.
- Run-twice determinism AE=0; sync-validation delta vs the known baseline (0 new lines — derived
  barriers must still be correct after decomposition).
- Toggling each pass off/on produces the expected, artifact-free delta (and re-enabling returns to
  AE=0).
- Persistent-plan invalidation correctness under resize / hot-reload / rapid toggling.
- Tracy: recompile only fires on invalidation, not per frame; per-frame re-record cost ≤ the old
  per-frame `plan.build()` cost.

## Implementation notes + grounded blueprint (2026-07-25)

Grounded in the actual code (`render_pass.hpp`, `render_plan.hpp`, `render_graph.{hpp,cpp}`,
`resource_usage.hpp`, `renderer.hpp`, `geometry_pass.*`).

### What landed (first increment — the authoring layer)

`string-engine/include/string/vulkan/frame_graph.hpp` (+ `test/frame_graph_test.cpp`): the fluent
`FrameGraph` authoring facade — lean `ImageHandle`/`BufferHandle` over `resource_id` (no
generation), `pass(name).reads/.writes/.requires_/.color/.depth/.toggle/.raster/.compute`, and
`compile()` implementing the **graceful-degrade** policy (disabled producer → optional reads land
in `CompiledPass.fallback_reads`; `.requires_` reads transitively skip to a fixpoint), lowering
onto the existing `GraphBuilder`/`RenderGraph` planner for toposort/lifetimes. Header-only,
GPU-free, unit-tested — **does not touch the renderer**, so zero regression risk. This is M0
(handles) + the M1 authoring/semantics core.

### Key finding: decomposition DELETES machinery, not just tidies

`Pass` (`render_pass.hpp`) has accreted virtual hooks that exist ONLY because `GeometryPass` is one
pass doing many things: `record_compute` (frame-top compute), `compute_only`, the async trio
(`has_async_compute`/`record_async_compute`/`async_usages`), and the 04d two-phase set
(`breaks_scene_group`/`record_between`/`record_after_between`/`depth_resolve_target`). The 04d hooks
exist purely to interleave a compute step between two draw batches into the same MSAA targets from
*inside one pass*. Once `geometry.phase1`, `hiz.build`, `geometry.phase2` are THREE separate
registered passes each declaring usages (phase1 writes msaa color/depth; hiz.build reads the
resolved depth + writes the pyramid; phase2 writes msaa color/depth, reads the pyramid), the graph's
existing grouping + barrier derivation reproduces the same schedule with **no special hooks** —
`depth_resolve_target` becomes hiz.build's declared read of a graph-produced resolve target. So M2's
win includes retiring those `Pass` hooks.

### Decomposition map (GeometryPass god-pass → registered passes)

`shadow.cascades` · `sky` · `ibl.capture/prefilter/sh` (compute, `.run_if(sun_moved)`) ·
`gi.capture` (load-time, run-once) / `gi.relight` (`.run_if`) / `gi.debug` · `froxel.cull` (compute,
async candidate) · `hiz.build` (compute) · `geometry.phase1` · `geometry.phase2` · `transparency`.
Each declares its reads/writes; the sentinel targets (`COLOR_TARGET`/`DEPTH_TARGET`/msaa) are
imported.

### Execution-wiring plan (the sensitive, capture-gated part — NOT yet done)

- **M1 (wire + persistent):** an adapter runs a `CompiledFrame` in the renderer, REUSING the
  existing `ResourceStateTracker` for barrier derivation and the existing color-target grouping —
  the `FrameGraph` replaces the per-frame `RenderGraph`-from-`Pass::usages` build. Compile once,
  cache, **invalidate on toggle-change/resize/reload**, re-record per frame slot. FIRST wrap the
  EXISTING passes as FrameGraph passes (record = current `Pass::record`), prove byte-identical, THEN
  proceed. Neutral fallbacks = a small registry of cleared images/buffers (0=black additive-neutral,
  1=white for shadow/AO); the neutral value is declared per resource at `image()/buffer()` creation.
- **M2:** extract each `GeometryPass` sub-pass into its own registered pass, one at a time,
  **byte-identical (AE=0) after each extraction** (03c precedent); retire the now-unused `Pass`
  two-phase/async hooks as their last user leaves.
- **M3:** bind `.toggle()` to CVars (`r.pass.<name>`); all-on ⇒ AE=0; each-off ⇒ the expected
  graceful-degrade delta.
- **M4:** introspection (enumerate passes + targets) + the handle-addressable mutable light/material
  store — consumed by briefs 14/15.

### GeometryPass decomposition strategy (2026-07-25) — TWO PHASES

The coupling is total: every sub-stage reads/writes shared `GeometryPass` members (meshlet buffers,
SceneData ring, worklists, camera, sun, bindless slots, shadow/hiz/gtao images), recorded across
`record_compute → record → record_between → record_after_between` in a precise hand-barriered order.
So decomposition is sequenced to never break the renderer:

- **Phase 1 — internal decomposition (BYTE-IDENTICAL by construction).** Introduce a shared
  `GeometryScene` (common resources) and split the monolith into member *components*, each owning its
  own state + record methods and referencing the scene. `GeometryPass` becomes a thin orchestrator
  calling components in the SAME order with the SAME barriers. Pure code movement → no behavior change
  → each step verifiable by capture-diff (AE=0). Dissolves the god-object; most of the modularity
  value lands here.
- **Phase 2 — promote components to registered graph passes.** Each component becomes a real pass
  declaring usages; the graph orders them; the `breaks_scene_group`/`record_between`/
  `record_after_between`/`depth_resolve_target` hooks get deleted. Riskier grouping/barrier work, done
  only after Phase 1 is solid.

### Record-flow map (the order Phase 1 must preserve exactly)

`record_compute` (frame-top, outside rendering): ensure_hiz (descriptor updates — MUST precede any
set bind) → **gtao** (prev-slot depth) → **ibl-update** (amortized) → **gi-capture**(once)/
**gi-relight**(amortized) → stats reset → **draw-cull** + **expand** (camera opaque/twosided +
per-cascade shadow lists) → two_phase decision + visbits clear → **shadow-cascades** (6× begin/end
rendering into cascade maps). `record` (inside scene MSAA group): **sky** → phase-1 opaque (two-phase)
OR single-pass opaque + probe-debug + transparency. `record_between` (outside): bitfield barrier →
depth-resolve transition → **hiz** pyramid build. `record_after_between` (reopened MSAA group): phase-2
opaque + probe-debug + transparency. `record_async_compute`: **froxel** binning.

Candidate component split: `SkyComponent`, `ShadowComponent`, `IblComponent`, `ProbeGiComponent`,
`FroxelComponent`, `HizComponent`, `GtaoComponent`, `MeshletDrawComponent` (draw-cull/expand +
phase1/2 opaque), `TransparencyComponent`. Streaming/residency + meshlet-buffer ownership + SceneData
ring + camera/sun live in `GeometryScene`.

### Naming debt (follow-up)

`FrameGraph` (fluent author-facing) vs `RenderGraph` (planner output) vs `GraphBuilder` (low-level
planner). Reconcile to one vocabulary once execution is wired — deferred to avoid a rename churn now.
