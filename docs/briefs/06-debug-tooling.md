# Brief 06 — Debug & profiling toolkit

Status: milestones 1+3(baseline)+CVar/Tracy slices DONE (prior); milestones 2, 3(HUD), 4, 5 IMPLEMENTED
2026-07-22 (branch gpu-layer) — surfaces landed: console, profiler HUD, debug-draw API + pass,
scene/draw inspector + isolate-draw, PNG capture + sequence + in-engine compare, stats overlay
consolidated into the HUD. flake check green; legacy .bmp recipe byte-identical + run-twice AE=0;
per-pass HUD numbers match the Tracy baseline (~9.4 ms interior GPU total vs ~9.5 ms). See the
"tooling surfaces slice" status section at the end. NEEDS VISUAL VERIFY: console/HUD/inspector
look-and-feel + debug-draw pattern (captures read by the agent, but aesthetic judgment is the user's).

> **REGRESSED 2026-08-07, RESTORED 2026-08-09 — per-pass GPU timing (milestone 3's HUD half).**
> Brief 20 (`12c1ebc`) deleted all **eight** `GpuProfiler::write_begin/write_end` call sites along
> with the per-pass recording paths they lived in, and the graph executor never picked them up. For
> two days `stats()` returned empty and `total_avg_ms()` returned 0 — while `enabled()` stayed TRUE,
> so the HUD took its success branch and printed **"GPU total 0.000 ms"** with no pass rows, and the
> DAG panel's `ms_of()` returned -1 for every pass. Nothing reported a failure. This "DONE" was false
> for that window.
>
> Restored at ONE site — `compiled_frame::execute`'s per-pass loop, the only place that now runs per
> pass — instead of the eight it used to take. Two limits are deliberate and recorded rather than
> hidden: **main-lane passes only** (the pool is reset on the main recorder, and an async pass on the
> compute queue could write timestamps before that reset executes; the profiler also tracks a single
> `open_slot`, which cannot express two lanes in flight), and `max_pairs` raised 64 → 128 because the
> graph now declares ~66 live passes and the old cap was silently dropping the tail.
>
> Verified by UI dump rather than by eye: 66 named pass rows, `GPU total 9.339 ms` on lookdev —
> consistent with this brief's original ~9.4 ms Tracy baseline.

## Goal

Promote the improvised debug levers that carried briefs 01–03 into first-class
tooling, built on the brief-05 UI system. Every later brief (PBR, VFX, post,
style checkpoint) is implemented AND verified through these tools — by the user
and by agents. This pulls forward spec #17 plus the tooling half of the old
style-checkpoint brief.

## Context (what exists, all improvised)

- `STRING_*` env vars scattered across passes (HIZ/LOD/CULL/LIGHTS/VIEW/CAM/
  CROWD/LOD_PX, CAPTURE_FRAME/CAPTURE_PATH, MESHLET_VALIDATE) — set-at-launch
  only, undiscoverable, no runtime toggling.
- BMP-only frame capture; imagemagick invoked by hand for diffs.
- Whole-frame CPU [frametime] log; GPU cost per pass has NEVER been measured.
- [mesh-cull] stats readback + a hand-rolled stats overlay; debug tints in the
  meshlet fragment shader; F freeze, C/O/L toggles hardcoded to keys.
- Tracy is vendored but not wired.

## Decisions (locked with user, 2026-07-22 reorder)

- **CVar system + console**: typed CVars (bool/int/float/string, help text,
  flags: cheat/persist/tier) registered where used; env `STRING_<NAME>` becomes
  a generic init-override so every existing lever and agent workflow keeps
  working; the debug keys (F/C/O/L, debug views) become CVar-backed bindings. A
  console panel (brief-05 UI): text entry with completion, history, log tail.
  This is the spec's Tier-5 console pulled forward.
- **Per-pass GPU timing**: `vkCmdWriteTimestamp2` pairs around every pass
  (prepass, HiZ, draw-cull, main, shadow cascades, froxel, sky, UI, composite)
  with a readback ring; rolling avg/worst per pass in a profiler HUD panel and
  in the [frametime] log line. **Wire Tracy** (CPU zones + GPU context) for
  deep captures — HUD for glanceability, Tracy for investigation.
- **Debug draw API** (moved from old style-checkpoint brief; Phase B needs it
  anyway): immediate-mode lines/AABBs/spheres/axes/text3d callable from any
  system, drawn depth-tested + overlay variants, backed by a per-frame vertex
  ring.
- **Scene/draw inspector** (brief-05 UI panel): enumerable draw table (name,
  meshlet/LOD counts, residency, material slots, AABB), click/hover to select →
  debug-draw AABB highlight + isolate-draw render mode (the per-draw-range
  bisection agents did by patching code becomes a CVar). Lights list with
  positions/ranges. Read-only v1 — live transform editing is out of scope.
- **Capture & diff first-class**: PNG capture (replace BMP) via console command
  + existing env trigger; capture-sequence support (every Nth frame); a
  `--compare` CLI/console path producing the AE pixel count + diff image
  in-engine (imagemagick optional, not required). Keep headless agent workflow
  (env-triggered capture at frame N then exit) — it is the agent verification
  backbone and must not regress.
- **Stats HUD consolidation**: one HUD (frame ms, per-pass GPU ms, draw/meshlet
  counters, streaming residency, VRAM) replacing the accreted overlay text.
- Live asset/shader swap over the wire, entity picking by server id: Tier-5
  stretch, NOT in this brief (shader hot-reload from brief 01 already covers
  the shader half locally).

## Milestones

1. CVar registry + env bridge + migration of all existing STRING_* levers and
   debug keys (zero workflow regressions — agent env recipes still work).
2. Console panel + log tail (on brief-05 UI).
3. GPU timestamps + profiler HUD + Tracy wiring; publish a per-pass cost
   baseline table for the standard test cameras (this baseline feeds every
   later brief's perf gate).
4. Debug draw API + scene/draw inspector + isolate-draw mode.
5. PNG capture/diff + capture-sequence; stats HUD consolidation.

## Acceptance

- Every pre-existing debug lever reachable via console AND env; F/C/O/L parity
  with old behavior (frozen-frustum capture diffs unchanged).
- Per-pass GPU baseline table reported for interior + exterior + crowd cameras.
- Inspector can find a draw by name, highlight it, and isolate it — demo'd on
  Sponza. Debug draw usable from any pass.
- Headless capture agent workflow verified end-to-end post-migration.
- Validation clean; flake check green; NEEDS VISUAL VERIFY on HUD/console/
  inspector look-and-feel (they use the new UI and should feel native to it).

---

## Status — profiling+CVar slice landed (2026-07-22, branch gpu-layer)

A pulled-forward slice of items (1) and (3) is implemented and verified end-to-end:

- **CVar system** (`string-engine/{include,src}/core/cvar.{hpp,cpp}`): typed self-registering
  console variables (bool/int32/float/string), process-global registry, `STRING_<NAME>` env
  bridge with legacy-name aliases, gtest-covered (`test/cvar_test.cpp`, wired into
  `test/meson.build`; runs under `nix flake check`).
- **Tracy profiler** (opt-in `-Dstring-engine:tracy=true`; no-op + non-dependency when off):
  extended macros in `core/profiler.hpp`; per-pass CPU+GPU zones named by `debug_name()`,
  frame mark, frame-time plot, worker-thread names, and zones across job system, file-watch,
  shader compile, residency, transfer batch. Tracy Vulkan GPU context on the graphics queue with
  per-frame `TracyVkCollect` on the recording buffer (frames-in-flight safe, non-blocking).
- **Engine proof-of-use**: frame-capture levers (`r.capture.frame` / `r.capture.path`) are now
  CVar-backed, honouring canonical `STRING_R_CAPTURE_*` and legacy `STRING_CAPTURE_FRAME/PATH`.
- **Verified**: `nix flake check` green; `.#demo` (no-op path) and `.#demo-tracy` both build and
  run headless with clean validation; Tracy client confirmed live (listens on :8086, live trace
  captured via `tracy-capture` — CPU+GPU zones present); Tracy overhead within noise on the
  interior camera (~9.5 ms both builds, viewer disconnected). NEEDS VISUAL VERIFY: nothing new
  visual in this slice (capture render unchanged from baseline).

Follow-ups still open in this brief: console panel + log tail (needs brief-05 UI), per-pass GPU
baseline table for the standard cameras, debug-draw API + inspector, sandbox env-lever → CVar
migration, and per-pass `debug_name()` overrides inside sandbox passes (currently only the
engine-owned `composite` pass names its GPU zone; sandbox passes show the default "Pass").

---

## Status — tooling slice 2 landed (2026-07-22, branch gpu-layer)

Delivers the sandbox env-lever → CVar migration, per-sandbox-pass `debug_name()` + inner per-stage
GPU zones, and the standing per-pass GPU baseline table below.

- **Sandbox lever → CVar migration** (`sandbox/debug_cvars.{hpp,cpp}`): every `STRING_*` `getenv`
  lever in `sandbox/**` is now a self-registering CVar under a canonical dotted name, with the
  LEGACY env token kept working verbatim via an alias. `register_debug_cvars()` runs first in
  `main()` (before the plan/passes build) and applies the env bridge. The only `getenv`s left in
  `sandbox/` are the resources-dir bootstrap in `main.cpp` (locates the config root; cannot be a
  CVar). Runtime key toggles (F/C/O/L/V/G/K) still mutate the pass members live — the CVar seeds
  the initial state. Migration is behaviour-preserving: all standard headless capture recipes
  produce byte-identical output (AE=0) to pre-migration baselines.

  | Canonical CVar        | Legacy env (alias)         | Type   | Default | Where used                        |
  |-----------------------|----------------------------|--------|---------|-----------------------------------|
  | `r.hiz.enabled`       | `STRING_HIZ`               | bool   | true    | geometry pass — HiZ occlusion     |
  | `r.lod.enabled`       | `STRING_LOD`               | bool   | true    | geometry pass — discrete LOD      |
  | `r.cull.enabled`      | `STRING_CULL`              | bool   | true    | geometry pass — GPU frustum cull  |
  | `r.lod.error_px`      | `STRING_LOD_PX`            | float  | 0.02    | draw-cull LOD error budget        |
  | `r.lights.enabled`    | `STRING_LIGHTS`            | bool   | true    | local-light stress set            |
  | `r.crowd.enabled`     | `STRING_CROWD`             | bool   | false   | crowd stress scene                |
  | `r.chunk.budget`      | `STRING_CHUNK`             | int    | 1024    | cooked chunk variant (0=unchunked)|
  | `r.debug.view`        | `STRING_VIEW`              | int    | 0       | meshlet debug view select         |
  | `r.camera.pose`       | `STRING_CAM`               | string | ""      | startup camera pose               |
  | `dbg.transp_test`     | `STRING_TRANSP_TEST`       | bool   | false   | synthetic transparency quads      |
  | `dbg.transp_reverse`  | `STRING_TRANSP_REVERSE`    | bool   | false   | transparency sort A/B check       |
  | ~~`dbg.meshlet_validate`~~| ~~`STRING_MESHLET_VALIDATE`~~ | — | — | **DELETED 2026-08-08 — registered but gated no code** |
  | `dbg.meshlet_readback`| `STRING_MESHLET_READBACK`  | bool   | false   | GPU-vs-CPU buffer memcmp           |
  | `dbg.meshlet_dump`    | `STRING_MESHLET_DUMP`      | int    | -1      | dump DrawInfo from index (-1 off) |

  (Engine-side `r.capture.frame`/`r.capture.path` ← `STRING_CAPTURE_FRAME`/`_PATH` landed in slice 1.)

- **Per-stage GPU zones**: `GeometryPass`, `Grid2DPass`, `UIPass` override `debug_name()`
  ("geometry"/"grid2d"/"ui"). Inside the geometry pass, `STRING_PROFILE_GPU_ZONE` scopes each
  logical stage so Tracy reads them by identity: `draw-cull`, `hiz-build`, `shadow-cascades`,
  `froxel-cull` (in `record_compute`) and `sky`, `main-draw`, `transparency` (in `record`).
  Engine API addition: `PassContext` now carries `STRING_PROFILE_GPU_CONTEXT_TYPE* gpu_profiler_ctx`
  (address of the renderer's Tracy GPU ctx, created post-`plan.build`; passes store the pointer and
  read it live at record time). No-op + zero-cost without `-Dtracy`.

## Per-pass GPU baseline (2026-07-22)

Method: `.#demo-tracy` headless (Tracy client on :8086), settled window (~8 s post-frame-300,
`STRING_LIGHTS=0`), `tracy-capture` → `tracy-csvexport -g`, mean of the per-frame GPU zone
"GPU execution time". Standard cameras. This is the standing per-pass perf reference; later briefs
gate against it. Numbers are software-rasteriser (lavapipe/dzn class) headless — treat as relative,
not absolute hardware ms.

Per-STAGE GPU mean (ms), the geometry pass broken into its zones:

| Stage             | interior | interior chunk=0 | exterior | crowd  |
|-------------------|---------:|-----------------:|---------:|-------:|
| draw-cull         |   0.031  |          0.022   |   0.031  |  0.71  |
| hiz-build         |   4.84   |          4.90    |   6.24   | 79.8   |
| shadow-cascades   |   0.74   |          0.41    |   0.77   | 24.9   |
| froxel-cull       |   0.107  |          0.107   |   0.106  |  0.11  |
| sky               |   0.105  |          0.105   |   0.123  |  0.10  |
| main-draw         |   3.81   |          3.46    |   1.43   | 14.4   |
| transparency      |   0.142  |          0.141   |   0.014  |  0.16  |
| ui                |   0.042  |          0.041   |   0.033  |  0.05  |
| composite         |   0.042  |          0.042   |   0.028  |  0.04  |

(The renderer wraps `record_compute` and `record` in two separate GPU zones both named "geometry",
so the raw "geometry" aggregate double-covers the stages above; the per-stage rows are the
authoritative breakdown.)

### Where the interior "~6ms ivy" cost lives — hypothesis REJECTED

The standing hypothesis was that **shadow cascades dominate** the interior geometry cost. The data
does not support it: on the interior camera `shadow-cascades` is only **0.74 ms**. The interior
geometry GPU time is dominated by **`hiz-build` (4.84 ms)** and **`main-draw` (3.81 ms)** — the HiZ
depth prepass + pyramid reduce is the single largest stage, and the lit meshlet main draw is second.
Shadows, froxel binning, sky, and transparency are each sub-millisecond. The exterior camera shifts
the balance further toward `hiz-build` (6.24 ms, more visible occluder geometry feeding the pyramid)
while `main-draw` drops to 1.43 ms (fewer/ farther-shaded pixels). **Actionable takeaway for later
briefs: the HiZ prepass — a full depth-only re-draw of the resident set every frame — is the primary
GPU optimisation target, not the shadow path.**

### Chunking overhead breakdown (interior chunked vs `STRING_CHUNK=0`)

Chunked interior costs ~0.5–0.7 ms more GPU than unchunked, concentrated in:

- `shadow-cascades`: **+0.33 ms** (0.74 vs 0.41) — the dominant chunking cost
- `main-draw`: **+0.35 ms** (3.81 vs 3.46)
- `draw-cull`: **+0.009 ms** (0.031 vs 0.022)
- `hiz-build`: within noise (4.84 vs 4.90)

Note the unchunked variant has MORE meshlets (62,267 vs 53,615) yet lower shadow/main cost: chunking
splits draws into more, smaller draw records, so the overhead is per-DRAW dispatch/command overhead
(re-emitted across all 3 shadow cascades + the main pass), not per-meshlet work. This matches the
previously-observed ~1 ms chunking overhead — the shadow cascades, which re-walk the draw list three
times, are where it concentrates.

### UPDATE (2026-07-22, brief 04d two-pass occlusion CLOSED) — the hiz-build prepass is GONE

Brief 04d replaced the full-set depth-only HiZ prepass with two-phase temporal occlusion (phase-1
renders last-frame-visible meshlets directly into the MSAA targets; the pyramid MIN-resolves from that
depth; phase-2 tests the complement against the fresh pyramid). The "primary GPU optimisation target"
flagged above (the `hiz-build` full re-draw) is eliminated. New per-STAGE table (same `.#demo-tracy`
method; new zone `phase2` = the disocclusion complement pass; `hiz-build` is now just MSAA MIN
depth-resolve + pyramid reduce):

| Stage           | interior c=0 | exterior | crowd  |
|-----------------|-------------:|---------:|-------:|
| draw-cull       |    0.016     |  0.032   |  0.225 |
| expand          |    0.029     |  0.071   |  0.062 |
| hiz-build       |    0.121     |  0.165   |  0.126 |
| main-draw       |    4.027     |  3.359   |  5.751 |
| phase2          |    0.445     |  0.786   |  5.738 |
| shadow-cascades |    0.447     |  1.140   | 13.795 |
| transparency    |    0.202     |  0.035   |  0.151 |
| froxel-cull     |    0.121     |  0.127   |  0.118 |
| sky             |    0.110     |  0.144   |  0.110 |

`hiz-build` collapsed interior 4.90→0.121, exterior 6.24→0.165, crowd 79.8→0.126 ms; the recovered
visibility work reappears as `phase2` (a fraction of it). Raster+HiZ total (hiz-build+main-draw+phase2)
fell −52% interior, −55% exterior, −87% crowd. No camera regresses. See brief 04d M3 for the full
verdict and the two-phase [mesh-cull] stat semantics.

---

## Status — tooling surfaces slice landed (2026-07-22, branch gpu-layer)

Delivers milestones 2, 4, 5 and the HUD half of milestone 3 (per-pass GPU timing was baselined in an
earlier slice; this adds the in-game readout). All built on the brief-05 UI kit.

### A) Console (M2)
`sandbox/ui/debug_console.{hpp,cpp}` (command engine, unit-tested logic) + the console panel in
`sandbox/ui/debug_panels.cpp`. Grave/backtick toggles it (raw key, works while text-captured);
`STRING_CONSOLE=1` / `dbg.console 1` opens it headless for capture. Modal: while open it sets
`Input::text_capture` and `InputMap` suppresses ALL gameplay actions (single choke point — camera +
debug keys go through the map; the console reads raw `Input`). Commands: `<cvar>` prints value+help,
`<cvar> <value>` sets (parses per type, honours legacy aliases), `help [prefix]`, `find <substr>`
(name+help search), `compare a.png b.png` (AE pixel count + diff image), `clear`. Tab-completes over
the CVar registry (LCP + candidate echo); Up/Down history. Log tail: an engine-side thread-safe ring
sink (`String::LogRingBuffer` in `core/logger.hpp`) mirrors every log line; the console renders the
last N, severity-coloured.

### B) Profiler HUD (M3 HUD half)
Engine `GpuProfiler` (`vulkan/gpu_profiler.{hpp,cpp}`): `vkCmdWriteTimestamp2` pairs around each
pass's `record`/`record_compute` at the renderer's dispatch seam (same place the Tracy GPU zones
sit), a query-pool ring per frame-in-flight, readback scaled by `timestampPeriod`, rolling avg/worst
keyed by `debug_name()`. Zero-cost-ish off (queries run; UI only when the HUD is shown). Per-pass ms
appended to the `[frametime]` log line every 600 frames. HUD panel (F2 / `dbg.hud`) shows per-pass
avg ms + GPU total, and consolidates the old mesh-stats overlay (one stats surface).

### C) Debug-draw API (M4 half)
Engine `string/debug_draw.hpp` — free functions `line/aabb/sphere/axes/text3d` + `_overlay`
variants, 8-bit RGBA, callable from any system, accumulating into a process-global per-frame ring
(`string::debug::context()`, cleared by the renderer each frame). Sandbox `DebugLinePass`
(`passes/debug_line_pass.{hpp,cpp}` + `shaders/debug_line.{vert,frag}`) drains the ring into a
per-frame-in-flight mapped vertex buffer and draws a world-space line list into the scene color after
geometry, before UI (depth-tested + overlay pipelines). Camera comes from the shared
`MeshOverlayStats` snapshot. `STRING_DRAW_TEST=1` emits a self-test pattern.

### D) Scene/draw inspector + isolate (M4 half)
Inspector panel (F3 / `dbg.inspector`) in `debug_panels.cpp`: scrollable draw table (index, meshlet
count, LOD count, resident flag) + lights list (position/range/type), from the per-frame snapshot the
geometry pass publishes into `MeshOverlayStats`. Hover/select -> debug-draw AABB highlight;
`dbg.isolate_draw` (`STRING_ISOLATE_DRAW`) holds a draw index that the draw-cull compute keeps while
zeroing all others. FENCE-ADJACENT: `meshlet_draw_cull.slang` + `DrawCullPush` — the isolate uint
REPURPOSES the former `_pad3` slot at offset 160, so ALL std430 offsets/size (192) are unchanged (no
static_assert edits); the shader zeroes `mcount` for non-isolated draws before the count writes.

### E) Capture polish (M5)
PNG capture: `core/png_writer.hpp` (dependency-free, deterministic stored-DEFLATE encoder); the
renderer capture path writes PNG when the path ends `.png`, else the byte-identical legacy BMP — so
existing `.bmp` recipes are untouched (verified: run-twice AE=0, `cmp` identical). Capture-sequence
`r.capture.every_n` -> numbered files. In-engine `compare` console command (AE + diff PNG via
stb_image read). Stats overlay consolidated into the HUD.

### Verification
- `nix flake check` green (incl. new `debug_draw_test.cpp`: debug-draw accumulation + PNG structure).
- Legacy `.bmp` recipe: `tools/capture.sh /tmp/x.bmp <cam>` still captures a valid 24-bit BMP;
  two runs are byte-identical (`cmp` -> AE=0), proving the timestamp writes don't perturb rendering.
- Captures read by the agent: HUD over Sponza (per-pass ms visible), console open (log tail +
  output), inspector with the draw table + lights + isolate highlight, isolate-draw (only draw 80
  renders), debug-draw test pattern (RGB axes + AABB + sphere, depth + overlay). All render correctly.
- HUD vs Tracy baseline (interior): `geometry (cs)` 5.3 ms ≈ baseline compute stages sum ~5.7 ms;
  `geometry` (graphics) 3.6 ms ≈ baseline sky+main+transp ~4.0 ms; GPU total 9.4 ms ≈ baseline
  ~9.5 ms. Same order of magnitude per stage.

### NEEDS VISUAL VERIFY (user, on ./run.sh)
- Console (grave): panel aesthetics native to brief-05? Tab-complete/history/set feel right?
- HUD (F2): per-pass numbers legible, panel sits cleanly (currently top-left below the demo panel).
- Inspector (F3): table readable; hovering a row highlights that draw's AABB in-world; isolate a draw
  and confirm only it renders.
- Debug-draw (`dbg.draw_test 1`): the origin test pattern reads clearly; overlay variants sit on top.
- Panel layout with the legacy "Hello, String!" demo panel — reposition/remove to taste (kept for now
  as the text-field interaction showcase).

## Per-pass GPU baseline — post-04e refresh (2026-07-23)

Method unchanged (`.#demo-tracy` + tracy-capture/csvexport, settled means) but
on REAL hardware (RADV/RDNA3), 1024x1024, `STRING_LIGHTS=0`, post-04d two-phase
pipeline + post-04e graph execution (STRING_ASYNC=1 default). Numbers are NOT
comparable to the 2026-07-22 software-raster table above — this supersedes it
as the standing reference.

| Stage            | interior | exterior | crowd  |
|------------------|---------:|---------:|-------:|
| draw-cull        |   0.015  |   0.013  |  0.227 |
| expand           |   0.025  |   0.025  |  0.057 |
| froxel-cull*     |   0.269  |   0.218  |  0.240 |
| shadow-cascades  |   0.389  |   0.381  | 11.637 |
| sky              |   0.189  |   0.235  |  0.210 |
| main-draw        |   4.398  |   1.670  | 11.385 |
| hiz-build        |   0.104  |   0.106  |  0.103 |
| phase2           |   0.326  |   0.161  |  3.209 |
| composite        |   0.070  |   0.044  |  0.066 |
| frame time       |   6.99   |   4.37   | 28.77  |

*froxel-cull runs on the async compute lane (04e M4); its zone lives in that
lane's own Tracy GPU context. See 04e's M4 gate section for the async on/off
deltas and the (currently zero) overlap finding.
