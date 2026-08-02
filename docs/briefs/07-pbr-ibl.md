# Brief 07 — Proper PBR: dynamic sky IBL, light units, exposure

Status: IMPLEMENTED (2026-07-23, M1-M4; furnace + numeric + determinism + multi-res + TOD +
sync-validation gates run — see the running log). NEEDS VISUAL VERIFY (lookdev scene + Sponza
TOD flythrough; list at the end of the log).

## Goal

Complete the physically-based half of the lighting model before VFX and post
build on it. Brief 02 delivered direct lighting (GGX/Cook-Torrance sun + froxel
lights, metallic/roughness/occlusion, Kaplanyan specular AA) over a two-color
hemisphere ambient — which is why metals and glossy surfaces go dead in shade.
This brief replaces that ambient with **dynamic sky image-based lighting**,
makes light intensities **self-consistent physical-ish units**, and lands a
proper **exposure model**. VFX emissives (brief 08) and bloom/auto-exposure
(brief 09) assume this HDR foundation.

## Current state

- `lighting.slang`: single shared shade_surface; HDR linear render target;
  `composite` does ACES tonemap with a hand-tuned exposure push constant.
- Ambient = hemispheric two-color (sky/ground) constants; no environment
  specular at all. Sun/light/ambient intensities are per-scene magic numbers.
- Procedural sky in `sky.slang`; full dynamic time-of-day is a locked
  requirement — **static baked IBL is off the table**.

## Decisions (locked with user, 2026-07-22 reorder)

- **Split-sum IBL** (industry standard): environment specular via a prefiltered
  cubemap mip chain (GGX importance-sampled, roughness ladder over mips) + a
  BRDF integration LUT; diffuse irradiance via **L2 spherical harmonics** (9
  RGB coefficients) projected from the same cubemap.
- **The environment is the live sky, updated at runtime**: render `sky.slang`
  to a small cubemap (128–256px), prefilter + SH-project on the GPU. Amortize:
  re-render/refilter spread over frames or triggered on sun-angle delta —
  budget it (<0.3ms avg). This generalizes to procgen worlds and dynamic TOD by
  construction; no per-level bakes ever. Local reflection probes / SSR are
  explicitly Phase-B-or-later; the sky IBL is the v1 ambient for everything.
- **Occlusion applied correctly**: baked AO multiplies diffuse ambient only;
  specular ambient gets horizon-based specular occlusion derived from AO
  (Lagarde), not a straight multiply; direct light is NOT AO'd.
- **Direct BRDF upgrade (locked with user 2026-07-23; reference implementation:
  Filament — use its shading-model doc as the authority when in doubt):**
  - Specular: GGX NDF (existing) with **height-correlated Smith visibility**
    (Heitz 2014; replaces the separable form in shade_brdf — kills grazing
    energy loss). Keep Kaplanyan specular AA on top.
  - Diffuse: **energy-renormalized Burley** (Lagarde/Frostbite variant, NOT raw
    Disney — raw Burley exceeds unity at grazing retroreflection) replaces
    Lambert for direct diffuse.
  - **Kulla-Conty multi-scatter energy compensation for direct specular, in v1
    not deferred**: implemented Filament-style from the split-sum DFG LUT this
    brief already builds (compensation = 1 + F0·(1/DFG.y − 1) — one sample of a
    LUT we already pay for; the expensive KC energy tables are unnecessary).
  - Fresnel: Schlick (existing) with **roughness-aware F90** (rough dielectrics
    must not get a white grazing sheen); F0 = lerp(0.04, albedo, metallic)
    stays, plus plumb Filament's optional `reflectance` dielectric-F0 remap
    (default 0.5 → 4%) for later stylized materials (gems/water).
  - **Roughness-convention audit is an explicit M1 task**: glTF roughness is
    perceptual (α = r²); direct BRDF, prefilter mip ladder, DFG LUT, and
    specular AA must all agree on the remap — verify before anything else, a
    silent mismatch here is the classic "IBL doesn't match direct on the
    sphere grid" bug.
- **Energy conservation audit**: single-scattering split-sum is the baseline;
  add Fdez-Agüera multi-scatter energy compensation for the IBL term (sibling
  of the direct Kulla-Conty term, same DFG LUT; closes the furnace test gap
  for rough metals). Validate with a **white-furnace test CVar** (uniform
  white environment → a white furnace scene should disappear). **Passing the
  furnace test is the acceptance gate for the BRDF work itself** (height-
  correlated Smith + renormalized Burley + KC compensation is exactly the
  combination that makes the furnace disappear), not just for the IBL.
- **Physical-ish light units, self-consistent**: sun in lux-scale intensity,
  point/spot in lumen-ish with inverse-square + range windowing (verify the
  existing falloff), sky luminance scaled to match the sun. **EV100 exposure**:
  manual EV CVar v1 (calibrated per test scene), pre-exposure applied so
  HDR16F headroom holds at midday sun. Auto-exposure (histogram adaptation)
  stays in brief 09 and slots into this EV model.
- **Tonemap**: keep ACES v1; revisit at the style checkpoint (brief 10) —
  tonemapper choice is a look decision, not this brief's.
- **Material probe scene**: a roughness×metallic sphere grid (plus a
  white/mirror pair) as a standing sandbox scene — the standard lookdev tool;
  used here for verification and forever after for regression eyeballs.

## Milestones

1. Sky→cubemap capture + GPU prefilter + SH projection + BRDF LUT (verify LUT
   + SH numerically against reference values; furnace CVar).
2. shade_surface ambient swap: SH diffuse + prefiltered specular + specular
   occlusion + multi-scatter. Probe scene + Sponza A/B captures.
3. TOD sweep: sun-angle-driven relight (sunrise→noon→dusk→night) with amortized
   update inside budget; no visible popping/lag in the ambient.
4. Units + EV pass: recalibrate sun/lights/sky/exposure to consistent values;
   retune the standard test scenes once; document the unit conventions in the
   brief.

## Acceptance

- Furnace test passes (uniform environment → flat response within tolerance).
- Probe scene reads correctly: full roughness ladder visible on metals in
  shade; dielectrics keep albedo; no energy gain at grazing angles.
- Sponza interior metals/glossy no longer dead; TOD sweep stays plausible at
  every hour with ONE exposure/unit calibration (no per-hour retuning).
- IBL update cost within budget (per-pass GPU timings from brief 06).
- Validation clean; flake check green; NEEDS VISUAL VERIFY at milestones 2–4
  (probe + Sponza screenshots; user judges material response and TOD sweep).

## Running log (2026-07-23 agent session)

### M1 — roughness-convention audit (FIRST task, findings)

Audited every roughness consumer before writing new code. Convention now locked everywhere:
**perceptual (glTF) roughness `r` at every interface; `alpha = r*r` applied only inside D/V and
importance sampling**. Findings in brief-02's shading:

- `distribution_ggx`: already correct (`a = roughness*roughness`, r perceptual). KEPT.
- `geometry_smith`: the UE4 *analytic-light* Schlick-GGX remap `k = (r+1)^2 / 8` — a separable
  form fed PERCEPTUAL r where the rest of the BRDF was alpha-based, and the (r+1) remap
  deliberately over-darkens. This is the grazing-energy loser the brief predicted. REPLACED by
  height-correlated Smith visibility (Heitz 2014), alpha = r^2.
- Kaplanyan specular AA: `r' = sqrt(r^2 + kernel)` on perceptual r — this MATCHES Filament's
  implementation (variance added in the perceptual-squared = alpha domain, clamped 0.18). KEPT
  unchanged; the ladder/LUT lookups after it use the same perceptual r'.
- Ambient (pre-07): no convention at all — `lerp(sky(R), irradiance, roughness)` plus an
  arbitrary `kIblStrength = 0.5` fudge. DELETED with the hemisphere ambient itself.
- New code (ibl.slang prefilter ladder, DFG LUT, lighting.slang eval): ladder mip m <->
  perceptual r = m/(mips-1); DFG LUT y-axis is perceptual r; both alpha = r^2 inside. One
  convention, three consumers, verified numerically below.

### M1 — sky IBL chain + DFG LUT (implemented)

- `sandbox/shaders/ibl.slang` (new): five compute entry points off one Push —
  `capture_main` (sky_env -> 128px RGBA16F cubemap mip 0; furnace flag writes uniform white),
  `mip_main` (2x2 average chain, 6 mips — the prefilter's PDF-mip source + SH source),
  `prefilter_main` (GGX importance-sampled ladder, 64 samples/texel, PDF-based source-mip
  selection, N=V=R; mip 0 = mirror copy), `sh_project_main` (single-workgroup L2 SH projection
  of the 16px capture mip, solid-angle weighted, cosine-convolved and pre-divided by pi so
  reconstruction is E/pi), `dfg_main` (128px split-sum LUT, 1024 samples, height-correlated
  Smith — the SAME visibility as the direct BRDF, so Kulla-Conty and Fdez-Aguera read off it).
- `sky.slang`: new `sky_env()` = gradient + glow, NO sun disc — the analytic (shadowed) sun is
  the direct light; capturing its disc would double-count it and alias in the prefilter.
- Resources (GeometryPass::create_ibl_resources): two 128px RGBA16F cubemaps (capture chain +
  prefiltered ladder, 6 mips each), 9x float4 SH buffer, 128px DFG LUT; SamplerCube slots via a
  new aliased bindless view (`SamplerCube lit_cube_textures[]` at binding 1 — descriptor
  aliasing); per-mip 2D_ARRAY storage views like the HiZ pyramid; dedicated clamp-to-edge
  trilinear sampler (the allocator default REPEATs — would wrap the LUT edges).
- Engine additions: `image_info.cube` (6 layers + CUBE_COMPATIBLE + CUBE default view) in the
  resource allocator; `ImageTransition.layer_count`.
- Graph declarations (04e authoring contract): SH buffer declared `{StorageWrite, COMPUTE}` on
  update frames + `{StorageRead, FRAGMENT}` every frame — the compute->fragment and cross-frame
  edges are DERIVED. The cubemap/DFG images use intra-pass barriers (the documented local class,
  same as the HiZ mip chain); the cubemaps live permanently in GENERAL so cross-frame WAR is one
  execution-only memory barrier. Zero new hand barriers outside the allowed classes.
- Slang gotcha discovered: a FIXED-size array (`float4 c[9]`) inside a pointer-loaded struct
  ICEs slang 2026.12 (slang-ir.cpp:5677 structType assert) when the enclosing struct is copied
  by value. Unsized `float4 c[]` (the existing LightBuffer/FroxelBuffer style) compiles fine.
- SceneData extended (APPENDED tail: `sh` pointer @680, env_slot @688, env_mips @692, dfg_slot
  @696, sizeof 704) — verified against `%SceneData_natural` OpMemberDecorate offsets via slangc
  spirv-asm; static_asserts updated in lighting_data.hpp. IblPush std430 offsets verified the
  same way (src_slot @64, sh @96).

### M1 gate — numeric verification (PASS, dbg.ibl_verify readback vs CPU references)

- Furnace capture SH: DC reconstructs E/pi = **1.00096** (expect 1.0; the 0.1% is the 16px
  solid-angle discretization), max |l>0 coefficient| = **0.00000**. PASS.
- DFG LUT vs a double-precision CPU integral of the same estimator at 5 probe points: all match
  within 2e-4 (e.g. (NdotV 0.5, r 0.5): gpu (0.8320, 0.0215) vs ref (0.8321, 0.0215)). A+B in
  [0.3118, 1.0009] over the whole LUT (single-scatter directional albedo <= 1 + half-precision
  epsilon; -> 1 as r -> 0). PASS. An INDEPENDENT random-sequence Monte-Carlo integrator (not
  the Hammersley code path) agrees to ~3 decimal places.
- Real-sky SH sanity: E/pi(+Y) = (0.19, 0.31, 0.57) blue-ish, E/pi(-Y) = (0.20, 0.17, 0.15)
  warm ground bounce, up > down. PASS.

### M2 — BRDF upgrade + ambient swap (implemented)

`lighting.slang` shade_surface, single shared module (3d/meshlet paths both import it — kept):

- Specular: GGX + **height-correlated Smith visibility**; Kaplanyan AA kept on top.
- Diffuse: **energy-renormalized Burley** (Lagarde/Frostbite energyBias/energyFactor variant,
  includes 1/pi) replaces Lambert for direct.
- Fresnel: Schlick with **roughness-aware F90 = max(1 - r, max3(F0))** (Lagarde) — rough
  dielectrics lose the white grazing sheen; used by direct AND the ambient split-sum combine.
  Dielectric F0 stays 0.04, written as Filament's `0.16 * reflectance^2` with reflectance 0.5
  (centralized for later per-material plumb).
- **Kulla-Conty energy compensation IN the direct specular**: `1 + F0*(1/(dfg.x+dfg.y) - 1)`,
  one LUT sample already paid for. No separate energy tables.
- Ambient: SH diffuse (E/pi) + prefiltered ladder specular + **Fdez-Aguera multi-scatter**
  (FssEss/Ems/Fms/kD form — closes the rough-metal furnace gap by construction). AO rules per
  the brief: baked AO multiplies diffuse ambient only; specular ambient gets the Lagarde
  horizon-based specular occlusion; direct light never AO'd. `kIblStrength 0.5` fudge deleted.
- Local lights: falloff audited — the old `win^2/(d^2+1)` denominator +1 arbitrarily clamped
  near-field; now `win^2/max(d^2, 0.01^2)` (Karis window over physical inverse-square).
- Furnace lever `r.furnace` (STRING_FURNACE): uniform white capture + white sky background +
  albedo forced 1, AO 1, sun + local lights skipped (SceneData.debug_flags bit 1).

### M2 — lookdev scene (implemented)

`STRING_SCENE=lookdev` (demo_scene.cpp; near-instant startup, no glTF): GeometryPass grows a
`lookdev` mode that generates the probe scene through the SAME bake library (bake_scene) —
8 roughness columns x 5 metallic rows of spheres, the white/mirror pair on the ground in front,
and a 40%-grey ground slab. Factors drive the materials (no textures); vertices pre-transformed
(world-space DrawInfo centers, cooked-scene convention). Stress lights default OFF in lookdev
(STRING_LIGHTS=1 re-enables). All existing levers work: TOD scrub keys, STRING_TOD pin,
STRING_ORBIT, furnace, captures. Standard lookdev camera used for gates:
`STRING_CAM="0,4,12,-1.5708,-0.05"` at 800x800.

### M2 gate — white furnace (PASS)

`STRING_SCENE=lookdev STRING_FURNACE=1`: the sky, ground slab, the metallic-1 row, the
dielectric row and the white/mirror pair **disappear into the uniform background** (pixel-flat
at the background value). The three MID-metallic rows (m = 0.25/0.5/0.75) remain faintly
visible: expected and correct — the glTF metallic model makes semi-metals absorptive
(kD scales with (1-m) while F0 = lerp(0.04, albedo, m); Filament documents the same). The
canonical furnace rows (m=0 albedo-1 dielectric, m=1 metal) vanish at every roughness — the
height-correlated Smith + renormalized Burley + KC/Fdez-Aguera combination is doing exactly
what it was locked in to do.

### M3 — amortized TOD updates (implemented)

- Trigger (update()): re-run the chain when the sun moved > 0.1 deg since the last capture
  (cos threshold), the furnace lever flipped, or first frame; `dbg.ibl_every_frame` forces the
  worst case. The WHOLE chain (capture -> mips -> SH + prefilter) runs in ONE frame, so the
  ambient is always self-consistent — amortization cannot pop mid-update by construction.
- Static sun: **1 update total** over 600-frame runs (logged `[ibl] frame N: 1 env updates`).
  TOD scrub/animation: every frame while the sun moves, nothing after it stops.
- Headless TOD levers added: `r.tod` (STRING_TOD pins time-of-day) + `dbg.sun_animate`
  (STRING_SUN_ANIM pre-arms the T toggle) for deterministic TOD captures/sequences.

### M4 — physical-ish units + EV100 exposure (implemented)

Unit convention (documented here, the brief's ask): **1 unit = 1000 photometric units** —
illuminance in KILOLUX, luminance/radiance in KILO-NITS, luminous intensity in KILOCANDELA.
The x1000 factor IS the pre-exposure: midday values sit ~1e2, comfortably inside HDR16F.

- Sun: 100 klx perpendicular at noon -> 7 klx at the horizon (warm color unchanged).
- Sky radiance: zenith ~6 knits at noon (2.8, 6.0, 12.4), ground bounce ~2.4 knits; dusk ~0.5.
  Consistency: pi * mean sky radiance ~ 15-25 klx diffuse skylight vs 100 klx direct — the
  right clear-day ratio. `sky_gradient`'s horizon band rewritten scale-invariant (the old
  absolute 0.85 constant would have become a dark band); visible sun disc clamped at 2000
  knits (tonemaps to white; fp16 is the constraint), glow x20.
- Local lights: kilocandela with true inverse-square (falloff fix above); stress set retuned to
  floodlight-class 150/300 kcd so it still reads against daylight.
- **EV100 exposure**: `r.exposure.ev100` CVar (STRING_EV100), exposure =
  1000 / (1.2 * 2^EV100), applied in the composite. Default **14.6 = sunny-16**, calibrated so
  an 18% grey card in noon sun lands on middle grey through ACES (verified on the lookdev
  captures). Tonemap stays ACES (brief-10 decision). Auto-exposure slots into this in brief 09.
- **Capture-path fix**: `capture_color_target` used an exposure-less Reinhard — every headless
  capture would have lied about the calibration. It now applies the SAME EV100 exposure + ACES
  as the composite (captures are directly comparable to the screen; this intentionally changes
  capture bytes vs pre-07 — this brief is not parity-gated).
- One calibration, no per-hour retuning: the TOD sweep gates below run at fixed EV 14.6.
  Interiors and dusk are correspondingly dark (physically honest at sunny-16); the EV CVar is
  the manual lever until brief-09 auto-exposure.

### Gate results (2026-07-23, pinned store + shader snapshot, machine otherwise idle)

Baseline pre-07 captures live at /tmp/07/base (pinned to the pre-07 store + git-index shaders);
post-07 gates at /tmp/07/gates. AE=0 parity vs baseline is NOT expected — this brief changes
shading; the baselines are the A/B pair for the user's visual judgment.

- **Furnace (the BRDF acceptance gate): PASS.** `STRING_SCENE=lookdev STRING_FURNACE=1`: sky,
  ground slab, the m=0 albedo-1 dielectric row, the m=1 metal row (every roughness) and the
  white/mirror pair all disappear into the uniform background (contrast-stretched inspection:
  pixel-flat). Only the mid-metallic rows (m=0.25/0.5/0.75) deviate — the glTF metallic model's
  intentional semi-metal absorption (diffuse weight (1-m) with F0 = lerp(0.04, albedo, m)),
  worst at high roughness, exactly as Filament documents. Whole-image stddev 2.4/255 at 800x800
  and 1.8/255 at 1280x720, all of it in those rows.
- **Run-twice determinism: PASS.** lookdev / interior / exterior 800x800 re-captures AE=0.
- **Multi-res: PASS.** lookdev + interior + exterior x {800x800, 1024x1024, 1280x720,
  1920x1080(->1131 display clamp)} all render sanely (histogram-checked; no black/garbage).
- **TOD stills** (STRING_TOD 0.02/0.15/0.30/0.50/0.75/0.95, lookdev + Sponza exterior, one EV):
  dawn-blue -> full daylight -> warm dusk, sun glints track, metals keep their ladder in shade.
- **TOD motion sequences** (STRING_SUN_ANIM=1, STRING_FIXED_DT, every 20th frame, ~270/190
  frames): frame-to-frame MAE is smooth; the ONLY spikes are the day-wrap discontinuity
  (time_of_day fmod 1.0 -> dusk-to-dawn jump at the 2000-frame period, a real scene cut) and
  the fast sunrise/sunset frames adjacent to it. Zero isolated spikes elsewhere -> **no popping
  from the amortized refilter** (expected: the chain updates whole-frame, never partially).
- **Sync validation** (VK_LAYER_VALIDATE_SYNC=1, 1024x1024: interior, lookdev with
  dbg.ibl_every_frame=1, interior TOD-animating): exactly the **4 pre-existing
  vkAcquireNextImageKHR WAR lines** in every run — **zero new hazard lines**. Normal validation:
  0 non-dzn lines across all 30+ capture logs (one mid-session lesson: the bindless set has no
  UPDATE_AFTER_BIND, so the IBL chain must record after ensure_hiz()'s descriptor updates —
  fixed by hoisting ensure_hiz to the top of record_compute).
- **IBL update cost vs the <0.3 ms budget: PASS with margin.** Tracy per-pass (RADV/RDNA3,
  .#demo-tracy, settled means): steady-state full chain (capture + mips + SH + prefilter) =
  **0.148 ms** even re-running EVERY frame (lookdev, dbg.ibl_every_frame). Under real TOD
  animation the trigger fires ~1 frame in 5 (0.1 deg threshold): measured 0.172 ms on update
  frames x 192/1039 frames = **~0.03 ms amortized**. Static sun: exactly 1 update per session
  (instrumented). The one-time first update (includes the 1024-sample DFG bake) is 1.67 ms.
- **Per-pass table before/after** (interior / exterior, vs the 04e M4 table): main-draw
  4.39 / 1.62 ms vs 4.40 / 1.67 ms baseline — **main-pass delta ~= 0** (the split-sum ambient
  costs what the old procedural-hemisphere ambient cost); shadow-cascades 0.42/0.42 vs
  0.39/0.38 (+0.03, noise-level); sky/hiz/froxel/composite unchanged. New zones: ibl-update
  (above), dfg bake folded into the first update.
- nix flake check green (engine + cook gtests); .#demo == the pinned gate store.

### Async-compute observation (the 04e follow-up question)

The sky-IBL chain was NOT placed on async-compute-0 this brief. It does not gate cleanly today:
the renderer's derived queue-family-ownership transfer (04e M4) covers BUFFERS only, and this
chain's products are cube-map IMAGES (layout + QFOT image barriers would need deriving —
a contained renderer extension, filed as the follow-up). Note also 04e's finding stands: even
the existing froxel async chain shows zero measured overlap on this machine post-late-latch
(the two residual serializer candidates in the 04e log are unresolved), so there is no
measurable win on the table until that is understood — the 0.15 ms chain runs inline on main,
amortized to ~nothing. When the image-QFOT extension lands, `record_ibl_update` is already a
single self-contained recording hook with no same-frame dependencies (its only input is push
constants), i.e. exactly the shape `record_async_compute` wants.

### API / declarations added (04e authoring contract)

- GeometryPass usages: SH buffer `{StorageWrite, COMPUTE}` (update frames) +
  `{StorageRead, FRAGMENT}` (every frame) — compute->fragment + cross-frame edges derived.
- Env cubemaps + DFG LUT: intra-pass local barriers (documented allowed class, HiZ-chain
  precedent); cubemaps live in GENERAL, DFG parked in SHADER_READ_ONLY after the one-time bake.
- Engine: `image_info.cube` (cube-compatible images + CUBE default view),
  `ImageTransition.layer_count`, `CompositePass::exposure_scale()` (shared by composite +
  capture writer), capture writer now applies EV100 + ACES (was exposure-less Reinhard),
  `lighting.slang`/`ibl.slang` added to the shader install set (lighting.slang was MISSING from
  meson install — pre-existing gap, run.sh/capture.sh masked it by symlinking the repo).
- New CVars: r.furnace (STRING_FURNACE), r.tod (STRING_TOD), r.exposure.ev100 (STRING_EV100),
  dbg.sun_animate (STRING_SUN_ANIM), dbg.ibl_verify, dbg.ibl_every_frame.
  Scene selector gained `lookdev` (STRING_SCENE=lookdev; gate camera
  STRING_CAM="0,4,12,-1.5708,-0.05").

### NEEDS VISUAL VERIFY (user)

Lookdev scene (`STRING_SCENE=lookdev ./run.sh`, camera above or fly):
- Metal row (top): full roughness ladder readable — mirror sky reflection at r=0 smoothly
  blurring to matte at r=1, NO banding between ladder mips, no dead/black metals in shade.
- Dielectric row (bottom): albedo holds across the ladder; grazing angles show no white sheen
  on the rough end and no energy GAIN (rim brighter than the sky it reflects) on any sphere.
- White/mirror pair: white ball reads as diffuse white under sun+sky; mirror ball shows sky,
  horizon, ground and a clean sun glint.
- `[`/`]` TOD scrub: ambient + reflections track the sun smoothly, no stepping/popping (the
  IBL re-filters behind a 0.1-degree trigger), sunrise/sunset go warm, shadows track.
- STRING_FURNACE=1: everything but the three mid-metallic rows vanishes into flat grey.
- Failure looks like: visible mip seams on the mirror ball (cube face borders), sparkle on
  smooth metals under TOD scrub (prefilter fireflies), ambient lagging the sun, banded ladder.
Sponza (`./run.sh`), interior + exterior flythrough + TOD scrub (T / [ ]):
- Interior metals/glossy trim no longer dead in shade (the brief's headline fix) — compare
  /tmp/07/base/int_*.bmp (before) vs /tmp/07/gates/int_*.bmp (after).
- Overall exposure: sunny-16 calibration makes interiors physically dark; judge whether the
  look is acceptable until brief-09 auto-exposure (lever: STRING_EV100, default 14.6; 13.5 is
  ~a stop brighter).
- Curtains/ivy (cutout + two-sided + blend paths) shade correctly under the new BRDF.
- TOD flythrough: one exposure across the day, no ambient popping, no per-hour retune needed.

### Open questions / follow-ups

- Async image-QFOT derivation so the IBL chain can take the async-compute-0 lane (above).
- Interiors receive the FULL unoccluded sky IBL (no large-scale occlusion until brief-09 SSAO /
  Phase-B GI) — Sponza's interior is therefore brighter relative to reality; expected v1.
- Semi-metal furnace absorption is the glTF convention (documented, not a bug).
- slang 2026.12 ICE: fixed-size arrays in pointer-loaded structs (unsized-array workaround in
  ShBuffer; upstream-reportable).
- Local reflection probes / SSR explicitly Phase-B-or-later (locked in the brief).

### Ground-band TOD fix (2026-07-23, follow-up session)

Bug (user screenshot, Sponza arcade at blue hour): the sky's ground band was a near-constant warm
radiance, so while the sky hemisphere dimmed/cooled with TOD, the ground hemisphere kept radiating
noon-warm — and interior ceilings' SH irradiance is dominated by the ground band, so vaults glowed
gold at dusk.

Fix (Frostbite-style): `sky_ground_` is reinterpreted as a constant ground ALBEDO; the shader
derives its radiance per capture/draw as `albedo/pi * (E_sun + E_sky)` — `sky_ground_radiance()`
in sky.slang — where E_sun = current sun illuminance projected on the ground plane
(sun_color * sun_intensity * max(sun_dir.y, 0); dims + warms through sunset, zero below the
horizon) and E_sky = pi * mean upper-hemisphere radiance of the current zenith/horizon gradient.
Both the visible sky (`sky`) and the IBL capture (`sky_env`) go through the same helper, so the
background and the interior ambient stay consistent by construction. Plumbing: sun intensity rides
the previously padded `sun_color.w` (IblPush) / `_sp4` (SkyPush) — no struct size/offset changes,
no resource-declaration changes (04e contract untouched). `SceneData.ambient_ground` (unused by
the lit shader since the IBL swap, kept coherent) mirrors the same formula CPU-side.

Calibration: albedo = (0.0824, 0.0699, 0.0503), chosen so noon (t=0.5) reproduces the brief's
ground radiance constant (2.64, 2.28, 1.80) knits exactly. Lower than a typical 0.2-0.4 terrain
albedo because the noon sun (~83 klx of the ~100-113 klx total ground illuminance) dominates: the
old 2.4-knit band was only consistent with ~0.08 reflectance under full physical illuminance —
preserving the noon look won. Analytic TOD table (new vs old ground radiance, knits):
t=0.50 (2.64, 2.28, 1.80) vs (2.43, 2.09, 1.65)  — noon ~8% brighter (the calibration target)
t=0.90 (0.50, 0.41, 0.37) vs (1.06, 0.88, 0.70)  — dusk half as bright, desaturated
t=1.00 (0.085, 0.087, 0.100) vs (0.44, 0.33, 0.28) — night ~4.5x dimmer, cool instead of warm

IBL trigger audit: capture inputs are now sun_dir, zenith, sun_color, sun_intensity + a constant
albedo — all pure functions of time_of_day_, and sun_dir changes for ANY TOD change (azimuth
sweeps even at the clamped elevation floor), so the 0.1-degree sun-delta trigger still covers all
ground-band change; no new stale-cache path. nix build .#demo green. Visual TOD captures deferred
to the user (headless capture needs a visible window on this session; Xvfb has no Vulkan WSI).

NEEDS VISUAL VERIFY (user): the Sponza arcade vault at dusk is THE acceptance shot —
STRING_TOD=0.9 (and 0.98 for night) ./run.sh, interior cam "9,4.5,0,3.1416,0.05": the ceiling
ambient should dim/cool with the sky instead of glowing warm gold; STRING_TOD=0.5 should look
~unchanged from before (ground band ~8% brighter).
