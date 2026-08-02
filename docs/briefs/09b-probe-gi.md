# Brief 09b — Probe GI: relightable irradiance volume

Status: **FUNCTIONALLY CLOSED 2026-07-25** — GI quality acceptable at fine spacing, CSM
fixed + validated; runtime cost is the one open item, DEFERRED to the end-of-Phase-A perf +
quantization brief (see closeout note at the end of the running log). Started 2026-07-24.
**M0 + M1 user-verified 2026-07-24** (M1 took six
iterations — see running log; final shape: GPU-driven multiview coarse-LOD capture, analytic
cull radius, relocation + classification; user: "placement looks good, dark spots plausible,
successful"). M2 user-verified ("plausible; passing"). M3 shading integration landed + relit; now
in the **M3 defect-fix phase** — round 1 (2026-07-24) fixed the oversized/view-dominant sampling
bias (swimming + a leak vector), corrected the Chebyshev query point, removed two leak-permitting
weight floors, trimmed the per-fragment fetch cost, and added a `r.gi.debug` GI-isolate view.
Round 2 (2026-07-24) root-caused the remaining leaks + the under-energized (near-black) GI view:
the capture was rasterized from UN-relocated probe positions while every consumer used relocated
ones — fixed with a two-round bake (re-capture from relocated positions); also fixed the probe
pipeline destructor leak. Awaiting live verify. Running log at the end of this file.
**Inserted ahead of 08 VFX (user
decision 2026-07-24)**: the 09 defect review exposed the last structural lighting
gap — interiors are lit as if open to the sky (Sponza pillars/arcades under the
roof receive the full sky probe; GTAO's ~1m radius cannot represent meters-scale
occlusion). Phase A order is now 07 → 09 → **09b** → 08 → 04f → 10.

## Goal

Scene-scale **diffuse** GI from a probe volume that is cheap at runtime and
correct under the dynamic sun: probes store *static visibility + capture-time
surface data* (rasterized once at load), and a tiny compute pass **relights**
them from the live sky/sun whenever time-of-day moves. Sponza's interior must
read as an interior at every time of day.

This is the classic pre-RT industry pattern (Enlighten, AC/Far Cry sky-visibility
probes): precompute the geometry/visibility part (static), recompute the lighting
part (dynamic) — no ray tracing hardware required.

**Phase 2 (explicitly OUT of scope, design for it, do not build it):** swap the
capture backend for `VK_KHR_ray_query` per-frame probe rays (full DDGI/RTXGI:
dynamic geometry participates, cheaper multi-bounce). The probe grid, atlas
formats, relight pass, and shading-side sampling must be backend-agnostic so
Phase 2 replaces *only* how capture texels are produced.

## Decisions (locked with user)

- **Diffuse only.** Split-sum sky IBL (brief 07) remains the specular ambient
  source unchanged. Probe-fed specular / SSR are future items, not here.
- **Static geometry only (v1).** Doors/characters neither occlude nor bounce GI;
  characters *receive* ambient from the probes (standard). Documented limitation.
- **Capture backend v1 = rasterized per-probe mini G-buffer cubemaps** (albedo,
  normal, distance; e.g. 6×32²–64² faces), captured at load amortized over
  frames. No HW RT anywhere in v1.
- **DDGI-style storage & sampling** (Majercik 2019 conventions): per-probe
  octahedral irradiance (8×8, R11G11B10F or RGB16F, 1-texel bilinear border) +
  visibility (16×16 RG16F mean/mean² distance, border) in atlases; shading does
  8-probe trilinear × Chebyshev visibility × normal/view-bias offsets ×
  backface weighting. The visibility test is what keeps outdoor probes from
  leaking into interiors — it is the point of the technique; do not simplify it.
- **Relight tracks TOD like the sky IBL does**: amortized on sun-delta threshold
  (brief 07 precedent), hysteresis blend on the irradiance atlas.
- **Volume**: uniform grid fit to the scene AABB v1 (spacing CVar, ~1–1.5m for
  Sponza; lookdev gets a volume too). Design the addressing so clipmap-scrolled
  cascades (procgen open world, Phase B) are a data-layout extension, not a
  rewrite. Outside the volume (or when total sample weight ~0): fall back to the
  existing sky-SH ambient exactly as today.
- **Prefer the standard technique at every fork** ([[prefer-standard-techniques]]
  — user's standing rule). Where this brief and the DDGI paper disagree, flag it
  in the running log rather than silently diverging.

## Technique spec

**Capture (load-time, amortized).** For each probe, rasterize the static scene
into a small cubemap G-buffer: albedo (post-texture, no lighting), world normal,
and distance. Reproject/collapse each probe's cubemap into a fixed per-probe set
of *capture texels* in an octahedral parameterization (direction → {albedo,
normal, hit distance | sky-miss flag}) stored in a static capture atlas. Also
produce the visibility atlas (mean/mean² of distance, DDGI-style) — this never
changes after load. Budget: amortize K probes/frame during load/streaming; no
single-frame hitch > ~2ms; log total capture time. A dedicated cheap raster path
(shadow-pass-style, albedo output added) is acceptable — do not drag the full
meshlet culling machinery through 6×N tiny views if a simple path is cheaper.

**Relight (runtime compute, the dynamic part).** For every capture texel:
`radiance = miss ? sky_env(dir) : albedo × (sun_direct + bounce)` where
`sun_direct = sun_color·intensity × max(N_hit·L,0) × shadow(hit)` and
`bounce = irradiance sampled from the probe grid at the hit point with N_hit`
(previous update's atlas → infinite bounce for free, the standard DDGI trick).
`shadow(hit)` v1: sample the CSM when the hit is inside cascade range (Sponza
fits in [near,200]), else treat as lit — note the far-field limitation in the
log. Then cosine-convolve texels into the octahedral irradiance atlas with
hysteresis. Units: everything stays in brief 07's kilo-unit/EV100 system —
probe irradiance must be dimensionally identical to the sky-SH term it replaces.

**Shading (lighting.slang).** In `shade_surface`, the ambient *diffuse* term
switches from sky-SH to the probe sample (weighted fallback to sky-SH as above).
GTAO continues to multiply ambient exactly as today (it is the sub-probe-scale
detail layer). `r.furnace` force-disables probe GI the same way it disables GTAO
(the furnace validates the BRDF against the analytic environment, not the GI).

**Scheduling.** Declare all new passes per the 04e authoring contract
(docs/briefs/04e "How to declare a pass" — MANDATORY). Relight+convolve is an
async-compute-lane candidate, but note the graph derives QFOT for buffers only —
image atlases crossing queues is the known image-QFOT follow-up; keeping the
chain on the main lane v1 is acceptable.

## CVars / debug

`r.gi` (on/off), `r.gi.spacing`, `r.gi.hysteresis`, `r.gi.probe_debug`
(instanced probe spheres showing live irradiance — also doubles as the
visibility sanity view), `r.gi.capture_face` (or similar inspection lever).
Debug panel entry with probe counts, atlas memory, relight cost, last update
age.

## Milestones

- **M0** — Probe volume + atlases + debug probe-sphere visualization (flat color
  first). Grid fit, addressing, memory report.
- **M1** — Capture: raster mini G-buffer cubemaps → capture + visibility
  atlases, amortized. Debug spheres show captured sky-visibility (interior
  probes visibly darker-sky than courtyard probes = the M1 acceptance image).
- **M2** — Relight + convolve compute with hysteresis; debug spheres now show
  lit irradiance tracking TOD (T key sweep).
- **M3** — Shading integration: leak-free sampling in `lighting.slang`, sky-SH
  fallback, GTAO layering, furnace gate green. **Pixel-parity checkpoint with
  `r.gi 0` vs pre-brief captures before anything is deleted/re-routed.**
- **M4** — TOD amortization, perf tables (`tools/tracy_pass_table.sh`), polish,
  docs, running log.

## Verification (per docs/briefs/README.md rules — all of them apply)

- Gates at multiple resolutions incl. non-square; run-twice determinism AE=0
  (use `STRING_FIXED_DT` + a fixed warmup frame count so hysteresis has
  converged identically); sync-validation delta vs the 4-line known baseline.
- `r.furnace 1` stays flat white.
- Perf budget: relight+convolve ≤ ~0.3ms amortized under TOD animation, ~0 when
  the sun is static; capture is load-time only.
- **NEEDS VISUAL VERIFY** (agent cannot see the screen — report exactly this
  list): (1) Sponza arcades/pillars under the roof at noon: plausibly dark,
  clearly darker than the courtyard — the defining before/after; (2) TOD sweep:
  interiors stay consistent (no noon-warm vaults at dusk); (3) leak check at
  thin geometry (curtains, walls with sky behind — layered-geometry rule);
  (4) lookdev + open courtyard essentially unchanged (probes must not fight the
  sky IBL outdoors); (5) probe-sphere debug view sane.

## Running log (2026-07-24 agent session)

### Architecture (locked in code)

- Probe state lives in `GeometryPass` (same home as the sky IBL — shares sun/TOD, sky-SH,
  scene AABB, camera). Shared structs/constants: `sandbox/passes/probe_gi.hpp`. Shared shader
  math (octahedral encode/decode, grid addressing, atlas tile UV, the DDGI 8-probe trilinear ×
  Chebyshev sample): `sandbox/shaders/probe_common.slang` — ONE implementation imported by
  relight, debug and (M3) lighting.slang so producer/consumer agree.
- **Atlas layout** (backend-agnostic for Phase 2): octahedral tiles packed 2D, X axis =
  (counts.x·counts.y) columns, Y axis = counts.z rows; each tile has a 1-texel DDGI gutter
  (irradiance 8→10 stride, visibility/capture 16→18). Four RGBA16F atlases: irradiance (relit),
  capture G-buffer (normal+dist), capture albedo, visibility (mean/mean² dist). Addressing is
  centralized in probe_common.slang `probe_tile_*` so a clipmap-cascade layout (Phase B) is a
  change there, not a rewrite.
- **Grid fit** (`fit_probe_volume`): uniform grid to the scene AABB, `r.gi.spacing` world-metres
  (0 = auto ~16 probes/longest axis), clamped `kProbeMaxPerAxis`=32 and `kProbeMaxTotal`=32768
  (coarsens spacing until it fits). Sponza at 1.5 m ≈ a few k probes; logs counts + atlas MB.
- **Passes** (all gated by `r.gi`): `record_probe_capture` (once) → `record_probe_relight`
  (amortized, sun-delta trigger identical to the IBL's cos(0.1°)) → `record_probe_debug`
  (instanced spheres into the scene MSAA group, after opaque, before transparency). Relight is
  ordered AFTER the IBL chain in record_compute so its sky-SH source is this frame's SH (explicit
  SH RAW barrier added; the IBL chain's own SH barrier only reaches FRAGMENT). Atlases live
  permanently in GENERAL with intra-pass barriers (documented local class, exactly like the IBL
  cubemaps) — not frame-graph-declared.
- **CVars**: `r.gi` (STRING_GI), `r.gi.spacing` (STRING_GI_SPACING), `r.gi.hysteresis`
  (STRING_GI_HYSTERESIS), `r.gi.probe_debug` (STRING_GI_PROBE_DEBUG: 0 off / 1 irradiance /
  2 visibility). `r.gi.capture_face` deferred to M1 (needs the raster capture to inspect).

### M0 — volume + atlases + debug spheres (implemented)

- All four atlases created + bound (sampled + storage slots), grid fit + memory report logged.
- Capture (M0): `probe_capture.slang clear_main` initializes capture texels to sky-miss and the
  visibility atlas to "far / variance 0" (Chebyshev never rejects) — a well-defined base so the
  chain is exercisable before the real raster lands (M1).
- Relight (M0): `probe_relight.slang relight_main` fills each irradiance texel with the flat
  sky-SH irradiance (E/pi) in the texel's octahedral normal direction, with hysteresis + the DDGI
  gutter border rule. This proves relight → atlas → sample end to end and tracks TOD already
  (the SH is the live sky's). M2 swaps the per-texel source for the capture-driven hemisphere
  integral (sun_direct + bounce); atlas/layout/sampling unchanged.
- Debug (M0): `probe_debug.slang` instanced procedural UV-sphere, one instance per probe, samples
  the probe's own irradiance (mode 1) or visibility mean distance (mode 2) on the GPU (no
  readback), depth-tested in the scene, ACES-ish tonemapped with the composite exposure scale.
- Gates: `nix build .#demo` green, `nix flake check` all pass, all four new Slang entry points
  compile clean under slangc with the engine's flags. NOT yet run live.

### M0 iteration 1 (user feedback: black spheres + too many probes)

- Debug spheres now have a dedicated **flat-grey mode** and a fixed-key lambert so they read as
  solid 3D balls regardless of atlas contents (M0 placement check). `r.gi.probe_debug`: 0 off,
  **1 flat grey**, 2 irradiance, 3 visibility. The user's "completely black" irradiance render is
  deferred to M2 diagnosis (the M0 flat sky-SH source should be non-black; the flat-grey mode
  decouples the placement check from that so M0 is verifiable now).
- Default `r.gi.spacing` raised 1.5 → **2.5 m** (Sponza ≈ 6630 → ≈ ~1.5k probes; the CVar tunes it).

### M0 iteration 2 (user feedback: too dark + probes-inside-geometry)

- **Root-caused the "completely black" irradiance spheres: double exposure.** The debug spheres
  draw into the scene HDR target, which the composite pass then exposes (EV100 ~0.03×) + ACES-
  tonemaps. Irradiance mode ALSO pre-multiplied by the exposure scale, so E/pi got exposed twice
  (~0.001 → black). Fix: irradiance mode now outputs RAW E/pi (composite does the one exposure,
  same as the rest of the scene); flat-grey + visibility modes divide by the exposure scale so
  their fixed greys are exposure-independent. Grey lifted to a readable light mid-tone.
- **Probes inside geometry (user question) — CONFIRMED a real waste + leak source, deferred to M1
  as probe classification/relocation (the standard DDGI fix).** A naive uniform grid drops probes
  inside Sponza's columns/walls; such probes have no valid irradiance and can leak if Chebyshev
  doesn't fully reject them. The industry-standard answer (Majercik probe states / RTXGI probe
  relocation + classification) needs the capture G-buffer to detect "inside geometry" (high
  backface-hit ratio) — so it lands in **M1** alongside the raster capture: mark inside/no-surface
  probes INACTIVE (relight skips them, sampling down-weights them) and optionally relocate them
  toward the least-occluded direction within their cell. Noted here so M1 picks it up.

### M0 — user-verified 2026-07-24 (flat-grey debug spheres pass).

### M1 — raster capture + visibility + classification (implemented)

- **Capture raster** (`probe_capture_raster.slang`, task+mesh+fragment, modelled on
  meshlet_shadow): per captured probe, renders ALL resident draws from each of the 6 cube faces
  into a small cube G-buffer — MRT0 albedo (post-texture, NO lighting), MRT1 world-normal.xyz +
  linear hit-distance(w), D32 reverse-Z depth. No cull (Sponza fits; dispatch grid =
  (meshlet_block, draw_index)). Face view-proj built CPU-side (`probe_face_view_proj`) to mirror
  ibl.slang `face_dir`, so the collapse `SamplerCube` reads agree with the raster writes.
  `kProbeCubeFace` face size, one cube reused across probes (serialized within the frame budget).
  Required a small engine addition: `pipeline_builder::set_color_formats()` for the 2-target MRT
  (single-attachment default unchanged, backward-compatible).
- **Collapse** (`probe_capture.slang collapse_main`, one 18×18 workgroup per probe): decodes each
  octahedral texel's direction, samples the cube G-buffer, writes {albedo, packed normal, hit
  distance | sky-miss(<0)} to the capture atlases and distance mean/mean² to the visibility atlas,
  incl. the 1-texel DDGI gutter (octa-seam reflection, same rule as relight).
- **Classification** (the user-requested inside-geometry fix): during collapse, each inner texel
  votes backface (dir·hit_normal>0 → probe sees the inside of geometry) or very-near hit; if
  >50% of directions vote, the probe is marked INACTIVE in a per-probe `probe_active_` buffer.
  Relight skips inactive probes (early-out). Sampling down-weight (M3) will read the same buffer.
  Probe *relocation* (nudging inactive probes to a valid spot) is left as an optional later
  refinement — the brief allows it; classification alone removes the wasted relight + is the
  leak-safety net.
- **Amortization**: K = `kProbeCaptureBudget` = 8 probes/frame; a cursor advances until it wraps,
  then `probe_captured_` latches and logs `[gi] probe capture complete: N probes, T ms`.
  Atlases are STATIC after that (never re-run unless the volume changes).
- **Latent M0 bug fixed by M1**: `ProbeRelightPush.probe_base/probe_count` were dead pads in M0,
  so the M0 relight dispatched over the wrong probe range (a no-op-ish). Now real fields
  (offsets re-verified; `sh`@144, `active`@152 static-asserted). This also improves M0's
  irradiance debug view.
- Atlases + cube G-buffer + activation buffer use intra-pass barriers (documented local class,
  like the IBL cubemaps) — not frame-graph-declared. Cleanup added to the destructor.
- Gates: all 8 probe Slang entry points compile clean; `nix build .#demo` green; `nix flake
  check` all pass. NOT run live.

### M1 iteration 1 (user feedback: unplayably slow during capture)

Capture at 8 probes/frame = 48 full-scene mesh renders/frame with NO cull → unplayable. Two
fixes: (1) the capture raster task shader now FRUSTUM-CULLS each meshlet against the face's 90°
view (only ~1/6 of the scene is in any one face cone → ~6× less mesh-shader work, matching the
meshlet_shadow cull); (2) amortization changed from probes/frame to FACES/frame
(`kProbeFacesPerFrame`=6 = one whole probe/frame), with a (probe,face) cursor — cube images move
to attachment at face 0, to SHADER_READ + collapse after face 5. Capture spreads over ≈ total
probes frames (~26 s for Sponza at 60 fps) and stays interactive throughout. Gates: raster slang
compiles, nix build green.

### M1 NEEDS VISUAL VERIFY — see the launch message.

### M1 — raster capture + visibility + classification (implemented 2026-07-24)

- **Capture backend = per-probe cube G-buffer raster.** New `probe_capture_raster.slang` (task+mesh+
  fragment, modelled on meshlet_shadow) draws ALL resident draws (no cull — Sponza fits a 90° face)
  into a reused 32² cube G-buffer: MRT0 albedo (post-texture base color, no lighting), MRT1 world
  normal.xyz + linear hit-distance(w), D32 depth (reverse-Z). One draw per cube face; dispatch grid
  = (meshlet_block, draw_index). Face view-projection is built in C++ (`probe_face_view_proj`) from a
  face basis (`probe_face_basis`) that mirrors ibl.slang `face_dir` so the raster's (face,uv) matches
  the collapse's `SamplerCube` read exactly.
- **Collapse = `probe_capture.slang collapse_main`** (one 18×18 workgroup per probe): each octahedral
  texel decodes its direction, samples the cube G-buffer, writes {albedo, packed normal, hit-dist |
  sky-miss(w<0 clear)} to the capture atlases and the distance mean/mean² to the visibility atlas,
  incl. the 1-texel DDGI gutter (same seam-reflection as relight).
- **Classification** (standard DDGI probe states): the collapse votes per inner direction on backface
  hits (`dot(dir, hit_n) > 0`) or very-near hits (<5cm); >50% ⇒ probe is INSIDE geometry ⇒ written
  INACTIVE in a new per-probe `probe_active_` device buffer (uint 1/0, default 1 via vkCmdFillBuffer).
  Relight skips inactive probes (early-out); M3 shading will down-weight them.
- **Amortization**: K=`kProbeCaptureBudget`=8 probes/frame, cursor advanced across frames; first call
  clears all atlases (clear_main) + zeroes activation; `probe_captured_` latches when the cursor wraps
  (logs "[gi] probe capture complete: N probes"). Atlases are STATIC thereafter. Cube G-buffer +
  activation live in GENERAL/attachment with intra-pass barriers (not graph-declared, like the IBL
  cubemaps + M0 atlases).
- **Plumbing**: `ProbeRelightPush` gained `probe_base`/`probe_count` (were dead pads — relight now
  dispatches the correct probe range; a latent M0 no-op is fixed) + an `active` device address
  (offsets static-asserted @144/@152). Engine: `pipeline_builder::set_color_formats()` adds MRT
  support (single-attachment default unchanged) for the 2-target capture raster.
- Gates: all 6 new/edited Slang entry points compile clean; `nix build .#demo` + `nix flake check`
  green. NOT run live — M1 acceptance image (interior probes darker-sky than courtyard) needs visual
  verify. Cube-face convention producer/consumer correctness is the main thing to eyeball.
- Deferred to M2/M3 as designed: relight still uses the flat sky-SH source (M2 swaps in the
  capture-driven sun_direct + bounce integral using these atlases); `r.gi.capture_face` inspection
  lever still open; probe RELOCATION (vs mere classification) left as an optional later refinement.

### M1 iteration 2 — GPU-driven multiview capture + probe relocation (implemented 2026-07-24)

Per user + lead: capture was laggy (per-face BeginRendering + a CPU per-draw loop of hundreds of
vkCmdDrawMeshTasksEXT), and probes sat half-in walls. Both fixed:

- **PART A — VK_KHR_multiview single-pass, GPU-driven capture.** All 6 cube faces now render in ONE
  `vkCmdBeginRendering` (`viewMask=0x3F`) over 6-layer 2D_ARRAY views of the cube G-buffer; the mesh
  shader picks the face view-projection by `SV_ViewID`. The whole probe is ONE
  `vkCmdDrawMeshTasksEXT(ceil(total_lod0_meshlets/32),1,1)` — a SINGLE flat GPU-driven dispatch over
  all LOD0 meshlets. The task shader (`probe_capture_raster.slang`) resolves each flat index to
  `{draw, global meshlet id}` via a load-time device table `probe_meshlet_draw_` (built once in
  `create_probe_resources` from `meshlet_model_`; no CPU per-draw loop), GPU frustum-culls the meshlet
  against all 6 face frusta into a 6-bit visibility mask, and amplifies survivors once; the mesh
  shader emits nothing for faces whose mask bit is clear. **NO CPU-side culling / per-draw work
  generation** (the hard requirement). The 6 face view-projections are built IN the shader from
  `probe_pos` (a fixed face-basis table) so the push stays at 64B — 6 mat4 would blow the 256B budget.
  Device multiview was NOT previously enabled; enabled `vulkan11_features.multiview` in device.cpp.
  Added `pipeline_builder::set_view_mask()` (sets `VkPipelineRenderingCreateInfo.viewMask`), called on
  the capture pipeline. Amortization is now PROBES/frame: `kProbesPerFrame=12` (a ~250-probe Sponza
  bake finishes in ~20 frames / a couple seconds, interactive throughout). The (probe,face) cursor
  collapsed to a plain probe cursor; each probe collapses right after its cube renders.
- **PART B — probe relocation (RTXGI-style).** The collapse (`probe_capture.slang collapse_main`) now
  computes a per-probe offset: it accumulates a push-away vector from near hits (backface hits push
  hardest = buried; close front hits push weighted by nearness), averages it, and clamps to
  ±0.5·spacing per axis so the probe stays in its cell. Stored in a new device buffer `probe_offset_`
  (float4/probe). The offset is applied via `probe_common.slang probe_world` (a new nullable
  `ProbeOffsetBuffer*` field on `ProbeGrid` — every consumer that calls `probe_world` sees the same
  relocated position, so it is wired for the M2 bounce + M3 shading path already) AND to the DEBUG
  sphere centres (so the user SEES probes move out of walls). A probe still >50% backface after the
  clamp stays INACTIVE (classification unchanged = leak safety net).
- **Default spacing** coarsened 5.0 → **2.5 m** now that capture is cheap (Sponza ≈ a few-hundred to
  ~1k probes; the CVar tunes it). Effective ~2.5 m spacing.
- **New resources**: `probe_offset_` (relocation), `probe_meshlet_draw_` (flat→{draw,meshlet} table),
  three 6-layer array views (replacing the 18 per-face views). All destroyed in the destructor; the
  offset buffer is zeroed + activation defaulted-active in the one-time capture init.
- **Gates**: all 8 probe Slang entry points + the raster task/mesh/frag compile clean; `nix build
  .#demo` green; `nix flake check` all pass. NOT run live.
- **TODO (M2/M3)**: relight (`probe_relight.slang`) still uses the flat sky-SH source and passes
  `offsets=nullptr` — when M2 wires the capture-driven bounce it should pass `probe_offset_`'s address
  into the relight push + `make_grid().offsets` so the bounce hit-point probe lookup uses the relocated
  positions. M3 lighting.slang `probe_irradiance` consumes `probe_world` already, so it inherits the
  offset once the buffer is plumbed into that ProbeGrid too.

### M1 iteration 3 — multiview fix + placement/leak polish (2026-07-24)

- **multiviewMeshShader feature enabled** (device.cpp). The prior iteration set `viewMask=0x3F` on a
  MESH-shader pipeline but only enabled the core `multiview` feature (vertex-pipeline only) — a
  validation error (`multiviewMeshShader feature was not enabled`) that left the capture pipeline
  invalid and was a real cause of the residual lag. This is the required companion feature.
- **Half-cell-offset grid + anisotropic Y** (`fit_probe_volume`, main-agent tips 2 + 6): the grid is
  now a FIXED-spacing lattice whose overshoot past the AABB is centred, so probes are inset ~half a
  cell from the AABB faces instead of planted flush in floors/walls/column axes (the first vertical
  layer hovers above the floor). Y spacing is 1.5x horizontal (architectural scenes vary less
  vertically) — a cheap probe-count win while keeping >=2 layers per room.
- **Backface-shortened visibility** (collapse, tip 5): a backface hit records distance x0.2 into the
  visibility atlas so Chebyshev aggressively rejects the probe for shading points on the far side of
  a wall — the DDGI second-line leak defense that makes imperfect placement tolerable.
- Default spacing 2.5 -> **3.0 m**; capture amortization **4 probes/frame** (each probe = one
  multiview dispatch over all LOD0 meshlets, GPU-culled to 6 faces — 4/frame bounds the load cost).
- Relocation (from iteration 2) + classification retained; tips 1 (always uniform), 3 (relocation),
  4 (kill hopeless probes = INACTIVE, skip relight) already in place.
- Gates: collapse slang compiles; `nix build .#demo` green. NOT run live.

### M1 iteration 4 — the actual bake-cost fix: distance culling (2026-07-24)

Diagnosed the real cost the user kept sensing: the capture dispatched ALL **122,779** LOD0 meshlets
(over 450 draws) for EVERY probe — O(all_meshlets × all_probes) ≈ 92M meshlet-visits for a 756-probe
Sponza bake. Per-face frustum culling was working, but it ran AFTER touching every meshlet.

Fixes:
- **Distance cull (the dominant win)**: a probe's diffuse GI is dominated by nearby geometry, so the
  task shader now rejects any meshlet whose sphere is entirely beyond `probe_cull_far_` (~6 cells,
  bounded by the volume diagonal) from the probe BEFORE any frustum math. Geometry beyond reads as
  open sky — the correct far-field diffuse behavior. This turns the bake into O(nearby_meshlets ×
  probes). Added `cull_far` to the raster push (offsets re-static-asserted; push 64→80B).
- **Groupshared frustum hoist**: the 6 face frusta depend only on probe_pos (constant across the whole
  dispatch), so they're now computed ONCE per workgroup into groupshared (threads 0..5 each build one
  face) instead of 6× per meshlet per thread — ~32× less cull arithmetic.
- Added a one-time `[gi] capture dispatch domain: N meshlets over M draws` log for future diagnosis.
- Gates: raster slang compiles; `nix build .#demo` green. NOT run live.

### M1 iteration 5 — coarse-LOD capture + honest cull radius (2026-07-24)

User: still laggy after iteration 4 (improved). CORRECTED cost model, two errors in iteration 4's
reasoning: (a) the 6 face cones tile the full direction sphere, so per-face frustum culling can
never shrink the surviving set — every in-range meshlet rasters into SOME face; (b) Sponza is only
~30 m across, so the "~6 cells" cull radius (~27 m) rejected nearly nothing. The dominant cost is
(meshlets-in-range × 64 vertex transforms) per probe — ~8M vertex transforms/probe at LOD0. Fixes:
- **Capture at each draw's COARSEST LOD** (dispatch table built from `lods[lod_count-1]`, not
  `lods[0]`): the cube faces are 32×32 px — GI capture on proxy/low-LOD geometry is the
  shipped-engine standard; the surface shift is far below the probe grid's resolution. Cuts the
  meshlet domain ~an order of magnitude (the dispatch-domain log now reports the coarse count).
- **Cull radius genuinely tightened**: ~3 horizontal cells with a 10 m floor (still spans the
  atrium height, so interior probes see roof, not sky), replacing the ineffective 27 m radius.
- Gates: `nix build .#demo` green. Result (user): bake "MUCH faster, genuinely realtime";
  122,779 → 19,620 meshlet domain. One minor residual hitch reported during the bake.

### M1 iteration 6 — analytic cull radius (user ask) + burst halved (2026-07-24)

- **Cull radius is now DERIVED, not tuned** (user: "can we analytically solve for it from grid
  spacing?" — yes): the visibility atlas only answers Chebyshev queries from shading points inside
  a probe's adjacent cells, so the exact upper bound on distances that must be captured is
  `r_vis = |spacing| (cell diagonal, worst-case query) + 0.5·max(spacing) (max relocation)
  + 0.75·min(spacing) (shading-time sampling bias)`. Geometry beyond r_vis cannot change any
  visibility result, and the radius scales with the grid automatically (tighter spacing → tighter
  radius → cheaper bake). At current Sponza spacing r_vis ≈ 10.7 m — the old 10 m floor was
  accidentally near-optimal, so this iteration's win is correctness + scaling, not a further cut.
- **Radiance caveat (the term that stops going tighter)**: the M2 relight reads first-hits beyond
  the radius as open sky. Fine for Sponza-class scenes (gallery ceilings are well within r_vis of
  their probes; the tall central atrium genuinely is open sky), but a scene with enclosed
  sightlines longer than r_vis would leak sky into the relight — revisit (separate radiance
  radius) if a future scene needs it.
- **kProbesPerFrame 4 → 2**: halves the per-frame bake burst (the residual-hitch lever); the bake
  doubles to ~6 s wall, still effectively load time.
- Gates: `nix build .#demo` green.

### M2 — capture-driven relight + convolve (implemented 2026-07-24)

- **Relight kernel rewritten** (`probe_relight.slang`): one 10×10 workgroup per probe, two phases.
  PHASE 1 (cooperative, into groupshared): radiance for each of the 256 capture directions —
  `miss → sky_env(dir)` (live sky, no sun disc); `hit → albedo × (sun_illum·NdotL·(1−shadow)/π +
  bounce)`. Shadow = **1-tap CSM** at the hit point from this frame's SceneData (pointer in the
  push; cascade select mirrors lighting.slang; outside cascade range → lit, the brief's documented
  v1 far-field limitation; no PCF at probe resolution). Bounce = `probe_irradiance()` of the
  previous atlas at the hit point with the hit normal (fallback sky-SH where probe weight ~0) —
  the DDGI infinite-bounce trick: each pass propagates light one bounce further. PHASE 2: cosine
  convolution `E(N)/π = Σ L·cos⁺·ΔΩ/π` (octa texels ~equal solid angle → factor 4/256; uniform L
  reconstructs exactly L, keeping the term dimensionally identical to the sky-SH it replaces),
  hysteresis blend, DDGI gutter rule. Imports `lighting` (SceneData/bindless/sh_irradiance) + `sky`.
- **Bounce self-read note**: the bounce SAMPLES the same atlas the dispatch WRITES (other probes'
  texels) — unordered old-or-new values, both valid iterates of the fixed-point solve the
  hysteresis smooths; accepted in lieu of ping-pong (flagged as standard practice).
- **Amortization**: `kRelightProbesPerFrame`=128, round-robin cursor. A sun trigger ARMS
  `kRelightConvergePasses`=32 full passes (bounce propagation + EMA settling; h=0.9 default →
  ~3% residual), then relight goes idle — ~0 static-sun cost, continuous under TOD animation
  (the trigger re-arms each frame the sun moves). Relight is gated on CAPTURE COMPLETION
  (partially-captured probes would relight from the cleared all-sky capture and read outdoor;
  also guarantees the sampled CSM maps have rendered at least once).
- **Plumbing**: ProbeRelightPush + `offsets` (relocated hit positions + bounce lookups) + `scene`
  (offsets@160, scene@168, sizeof 176, static-asserted); relight signature takes current_frame
  (SceneData ring slot). Irradiance atlas is TRANSFER_DST and zero-cleared at capture init
  (shading/debug may sample texels before their first relight). End-of-relight barrier extended
  with an EXECUTION edge to EARLY/LATE_FRAGMENT_TESTS: relight READ this slot's shadow maps at
  COMPUTE, the cascade pass recorded later this frame WRITES them (WAR).
- `r.gi.hysteresis` default 0.95 → **0.9** (0.95^32 left 19% stale energy at idle; 0.9^32 ≈ 3%).
- Gates: relight slang compiles (only the pre-existing intentional binding-alias warnings);
  `nix build .#demo` + `nix flake check` green. NOT run live.

### M2 — user-verified 2026-07-24 ("plausible; passing").

### M3 defect-fix round 1 — bias/leak/perf + GI-debug view (2026-07-24)

User live report (GI on, Sponza): geometry-locked artifacts whose VISIBILITY flips with camera
angle (no shimmer — "swimming"); light against a fully-occluded pillar side under the arcades
(a leak); a small CONSTANT framerate cost (independent of bake + sun animation); scene overall
darker with identical direct light (plausibly correct); nothing crisp to judge against. Fixes:

- **Sampling bias was ~10x too large and view-dominant (root cause of the swimming AND a leak
  vector)** — `probe_common.slang probe_irradiance`. The bias was `(N*0.2 + V*0.8) * 0.75*min_spacing`
  ≈ 2.25 m with a 4:1 view weight, so the query point swung up to ~1.8 m along the CAMERA direction:
  which probe/cell won flipped as the camera turned (the "swimming"), and the swing could push the
  query through a thin wall into a lit cell. Replaced with the RTXGI-standard SMALL, NORMAL-dominant
  bias: `N * 0.25 m + V * 0.10 m` (each capped to a fraction of the cell for tight grids). This is
  the [[prefer-standard-techniques]] value, not a tuned one.
- **Chebyshev query point corrected**: distance/direction for the visibility test now measured from
  the probe to the BIASED query point `p` (was the un-biased `world_pos`) — that is exactly the
  query the visibility atlas answers. Was very wrong under the old huge bias; correct now.
- **Two leak floors removed/reduced**: the backface wrap floor `wrap²+0.2` → `wrap²+0.05` (a probe
  behind the surface no longer keeps 20% weight); and the post-Chebyshev `weight = max(weight,1e-6)`
  floor DELETED (it let 8 wall-rejected probes sum to a small stale leak) — a Chebyshev-rejected
  probe now contributes exactly nothing; the total-weight≈0 sky-SH fallback handles "all rejected".
- **Perf: skip the two atlas fetches for strongly back-facing corners** (`weight < 1e-3 → continue`,
  before the visibility+irradiance SampleLevel). Roughly halves the average fetch count per fragment
  (the ~half of the 8 cell corners behind the surface) with no visible change — the constant-cost
  lever. VERIFIED relight is idle when the sun is static (gated on `probe_relight_pending_`, armed
  only on sun-delta), so the constant cost is the per-fragment sampling, not the compute.
- **GI-debug isolate view (new tooling)**: `r.gi.debug 1` (STRING_GI_DEBUG) makes `shade_surface`
  return ONLY the indirect diffuse (`probe irradiance × albedo`) — no direct, no specular ambient —
  so a reference-free scene becomes judgeable and the user can point at a wrong probe. Plumbed via
  `probe_gi==2` in SceneData (0 off / 1 normal / 2 debug).
- Gates: `nix build .#demo` + `nix flake check` green (probe_relight.slang, which imports the shared
  math for its bounce, recompiles clean too). NOT run live — needs the user's eyes (see below).

### M3 defect-fix round 2 — capture/relocation consistency (2026-07-24)

Round-1 user report: better but not fixed — (a) interior side of courtyard-row pillars still lit
(physically impossible); (b) blotch artifacts now stable under camera ROTATION but change with
TRANSLATION (consistent with the small view bias: V for a fixed surface point changes with camera
position, not rotation); (c) `r.gi.debug 1` shows nearly EVERYTHING dark with only the leak
blotches lit — the probe volume is drastically under-energized, not just leaking.

**Root cause found (structural): the capture was inconsistent with relocation.** The cube G-buffer
was rasterized from the UN-relocated grid position; the collapse derived a relocation offset
(clamped ±0.5·spacing = up to 1.5 m) from that same cube; but every consumer (relight hit
reconstruction `hit = probe_world + dir·dist`, the Chebyshev distance comparison in shading, the
bounce lookups) uses the RELOCATED position via `probe_world()`. So for every relocated probe:
relight reconstructed hit points up to 1.5 m off → the 1-tap CSM sampled wrong world points (often
reading "shadowed") → sun under-injected → dim/black irradiance broadly (the darkness); and the
shading Chebyshev compared distance-to-relocated-probe against a distance field measured from the
old position → over-rejection in some directions, leaks in others, position-sensitive (a+b).

**Fix: two-round bake (RTXGI-style relocation consistency).** Round 1 = as before (raster from
grid pos → collapse → offsets + classification). Round 2 = re-raster each probe from its RELOCATED
position and re-collapse (capture/visibility distances now measured from the position everyone
samples with); classification re-votes (an escaped probe can become ACTIVE), but the offset is NOT
rewritten (no compounding drift). Implementation: the raster shader now always adds
`offsets[probe_index]` to the push probe_pos — the buffer is zero-filled and a probe's own offset
is only written by its collapse AFTER its round-1 raster, so round 1 reads 0 and round 2 reads the
round-1 relocation, no flag needed in the raster. Collapse gained a `round2` push flag (gates only
the offset write). Capture cursor runs 2×total (bake wall time doubles, still load-time). The
collapse barrier gained TASK|MESH dst stages (round-2 task/mesh/fragment read the offset buffer).

Also fixed: the five probe shader programs (clear/collapse/relight/capture-raster/debug) were
missing from ~GeometryPass's destroy_program list — exactly the 5 leaked pipeline layouts +
pipelines in the user's shutdown validation errors. Added.

Gates: `nix build .#demo` + `nix flake check` green. NOT run live. Verify-round notes: the GI-debug
view is EXPECTED to look dim at the sunny-16 EV (indirect sits ~3-5 stops below direct) — judge
relative structure, or drop `r.exposure.ev100` a few stops while inspecting. The probe spheres
(`r.gi.probe_debug 2`) are the atlas-vs-sampling discriminator: bright plausible spheres + dark
surfaces = sampling bug; dark spheres too = relight bug.

### M3 — shading integration (implemented 2026-07-24)

- **SceneData probe tail** (lighting.slang + lighting_data.hpp, APPENDED — earlier offsets
  unchanged): probe_origin@784, probe_spacing@796, probe_counts@808, irrad/vis slots@820/824,
  probe_gi@828, probe_offsets*@832, probe_active*@840, sizeof 848 — **verified against
  %SceneData_natural OpMemberDecorate** (slangc spirv-asm) and static_asserted.
- **shade_surface ambient-diffuse swap**: when `probe_gi != 0`, `irradiance` (the E/pi term) comes
  from `probe_irradiance()` — the Chebyshev-gated 8-probe trilinear over the relit atlas — with a
  one-cell hull fade blending back to the sky SH outside the volume and a ~0-weight fallback.
  Specular ambient stays the sky IBL (locked decision); GTAO multiplies exactly as before (the
  sub-probe-scale layer); baked-AO rules unchanged.
- **probe_gi gating** (set in update()'s SceneData fill): 0 under `r.gi 0` (the pixel-parity
  lever — the shader then takes the pre-09b path bit-identically), 0 under `r.furnace` (same rule
  as GTAO), 0 until capture + the FIRST FULL relight pass complete (the atlas is zero-cleared
  before that — sampling it would darken instead of falling back).
- **Inactive-probe skip in sampling** (`probe_common probe_irradiance`): probes classified inside
  geometry contribute NOTHING (their texels never relight); `ProbeActiveBuffer` added to ProbeGrid
  (nullable), wired in relight-bounce + debug + shading. Relight's bounce also skips them.
- Gates: all affected Slang entry points compile (incl. the meshlet task/mesh/fragment chain that
  imports lighting); `nix build .#demo` + `nix flake check` green; SceneData natural offsets
  SPIR-V-verified. NOT run live — the headline visual (interiors darker at noon), the r.gi-0
  parity toggle, TOD consistency, furnace flatness and the leak check are the user's list.

## Phase 2 sketch (do not build)

Ray-query capture backend: BLAS at cook/load, TLAS refit, per-frame budgeted
probe rays replacing the static capture atlas; dynamic geometry then occludes
and bounces; multi-bounce quality improves. Clipmap scrolling volumes land with
Phase B procgen. Opt-in CVar tier, same atlases/relight/sampling.

## Closeout (2026-07-25, user)

- **CSM fixed + validated + done.** This session root-caused the shadow "sparkle"/leak
  (divergent bindless UB — per-pixel cascade slot needed `NonUniformResourceIndex`) and the
  meshlet CSM bug (`glm::ortho` emitted OpenGL [-1,1] depth in the geometry_pass TU → reverse-Z
  clipped sunward casters; fix `glm::orthoRH_ZO`). GI polish: NaN firewall, grid inset fit,
  soft-collapse fallback `r.gi.occluded_floor`, GI-derived spec-occ, `shaderInt64`, spacing
  default 2.0.
- **GI quality is acceptable only at fine spacing, where it is currently expensive per-frame.**
  User's read: not a good long-term cost/quality tradeoff as-is. Likely cause = the full-res
  per-fragment 8-probe × Chebyshev fetch (production DDGI samples GI at half/quarter res into a
  GI buffer — that optimization is unbuilt). GI stays gated behind `r.gi` and remains the seam
  for the Phase-2 HW ray-query backend (which changes CAPTURE / dynamic geometry, NOT the
  per-fragment cost). If GI is ultimately cut, the interior-lit-as-open-sky gap that motivated
  this brief REOPENS — decide against measured numbers, not feel.
- **Pixel-parity (`r.gi 0` vs pre-brief) RETIRED as a gate.** Fixing CSM changed the visual
  reference on purpose (better); same re-baseline logic as 04d — the new output is the baseline.
- **All measurement / perf / optimization DEFERRED** to a single end-of-Phase-A perf +
  quantization brief (absorbs 04f). That brief quantifies GI cost with
  `tools/tracy_pass_table.sh` (r.gi on/off), then decides optimize-vs-cut. M4 (perf tables) is
  folded into it. Phase A order: **09b (closed) → 08 VFX → [perf + quant brief] → 10 checkpoint**.
