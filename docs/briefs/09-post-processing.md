# Brief 09 — Post-processing chain

Status: IMPLEMENTED (2026-07-23; bloom + histogram auto-exposure + GTAO w/ bent
normals + ACES-2.0-style LUT tonemap + HDR grading + outline slot; gates in the
running log at the end of this file). NEEDS VISUAL VERIFY. **Four live-reported
defects fixed 2026-07-24 (GTAO banding, furnace darkness, dark mirror
reflections, over-aggressive auto-exposure) — see the fix session at the end of
the log; pending user re-verify.** SMAA (a locked
decision below) is NOT in this slice — deferred to a follow-up session, see the
running log. **PULLED FORWARD ahead of 08 VFX (user decision
2026-07-23): finish the visuals before adding effects — VFX emissives should be
authored against the FINAL bloom/tonemap/grading chain, not a temporary look.**
Phase A order is now 07 → 09 → 08 → 04f → 10.

(Moved from slot 05 in the 2026-07-22 reorder. The HDR/exposure-model/tonemap
foundation now lands in brief 07 (PBR) — this brief builds the chain on top of
it rather than defining it.)

## Goal

The stylized look locks in here: **tight & punchy bloom**, auto-exposure, GTAO,
**ACES 2.0 output transform + HDR color grading (baked into one 3D LUT)**, and
a reserved slot for an outline/rim pass — decided at the style checkpoint
(brief 10), not here.

## Current state

- HDR linear lighting with brief 07's EV100 exposure model and physical-ish
  light units; ACES tonemap in `composite`; swapchain is sRGB. No other post
  exists. Render-graph scaffolding exists (`render_graph.hpp` planner); use
  judgment on whether to formalize the chain through it or keep explicit pass
  ordering — don't let graph work balloon this brief.

## Decisions (locked with user)

- **Bloom: tight & punchy** — physically-based downsample/upsample chain
  (Karis-average firefly suppression), but artistically clamped: narrow-ish
  contribution, saturated glow that makes spells/emissives pop with no scene-
  wide haze. Threshold/radius/intensity as CVars (tuned at the checkpoint).
- **Auto-exposure**: luminance histogram compute + smoothed adaptation driving
  brief 07's EV100 target, with min/max EV clamps and the manual-override CVar
  (dynamic time-of-day makes fixed exposure untenable).
- **AO: GTAO, locked (user decision 2026-07-23, upgraded from "GTAO-or-simpler").**
  Half-res horizon-based GTAO (Jimenez/Activision 2016) with the multi-bounce
  approximation (avoids over-darkened corners) and **bent normals output** — the
  bent normal feeds brief-07's Lagarde specular-occlusion path (replacing the
  AO-derived approximation) so diffuse AND specular ambient get consistent
  screen-space visibility. Subtle strength; it supports the IBL ambient term,
  not a heavy darkening look. Strength CVar; off = tier option. Context: the
  single distant sky probe lights fully-occluded interiors (Sponza arcade
  vaults); GTAO is the screen-space dent in that, local probes/GI (Phase B)
  are the real fix. NOTE: the TOD-tracking ground-band fix (ground albedo x
  live irradiance, done post-07) is a separate radiance correction — AO
  attenuates ambient, it cannot fix ambient that is temporally wrong.
- **Tonemap: ACES 2.0 output transform, locked (user decision 2026-07-23 —
  supersedes "keep ACES v1, decide at checkpoint").** Implement via a **baked
  3D LUT** (bake the ACES 2.0 Rec.709/sRGB output transform once at init — the
  full transform is too heavy per-pixel; runtime cost = one 3D texture sample
  in composite). Keep the current ACES-v1 fitted curve behind a CVar for A/B
  at the style checkpoint. Motivation: 2.0 fixes the 1.x hue skews (red→orange,
  blue→purple near clip) that matter for a saturated stylized palette.
- **Color grading: HDR-space grading composed into the SAME LUT.** Grading ops
  (exposure trim, contrast, saturation, lift/gamma/gain, white balance —
  CVar-driven v1, artist LUT import later) are applied in the working space
  BEFORE the output transform when baking, so the composite still pays one LUT
  sample total. Re-bake on parameter change (amortized, like the sky IBL).
  Neutral grade ships as default; the style checkpoint (brief 10) tunes it.
- **Outline/rim: DO NOT implement a style** — implement the *slot*: a post-pass
  hook with depth+normal available where either rim-light or edge-detection can
  land after brief 10 decides. (Normals may require a thin G-buffer output or
  reconstruction — prefer reconstruction from depth v1 to avoid pass rework.)
- **SMAA as a switchable AA mode alongside MSAA** (decision 2026-07-22): the
  user's image-quality pillar is crisp/clean — no temporal smear. SMAA 1x
  (full standard: edge detect + blend-weight LUTs + neighborhood blend) on
  the resolved image, CVar-switched vs the current MSAA path so the style
  checkpoint (brief 10) can A/B them on real content. If SMAA passes the
  user's eye there, it unblocks a possible future visbuffer architecture
  (which is MSAA-incompatible but SMAA-compatible); visbuffer itself remains
  a separate, evidence-gated decision — NOT scoped here.
- **TAA/upscalers remain out of scope and TAA-dependent techniques are
  disallowed pipeline-wide** (no dithered transparency, no stochastic
  effects needing a temporal denoiser). A player-optional upscaler tier may
  be revisited post-Phase-A; never as the reference path.

## Milestones

1. Bloom chain + CVars (test against brief 08's emissive-heavy stress scene).
2. Auto-exposure + time-of-day sweep test (sunrise→noon→night stays readable).
3. SSAO + strength/quality CVars.
4. Outline-slot plumbing (no-op pass wired, depth/normal inputs proven).

## Acceptance

- Bloom pops on spells/emissives without hazing the whole frame (user judges);
  no fireflies/flicker in motion.
- Exposure adapts smoothly across the full time-of-day sweep; no pumping.
- SSAO subtle, no halos at pulled-back camera distance.
- Post chain total GPU cost reported; validation clean; flake check green.
- NEEDS VISUAL VERIFY at every milestone (this brief is entirely look-driven).

## Running log (2026-07-23 agent session)

### Architecture (what landed where)

- **Compute-only frame passes (renderer extension, in the 04e spirit):**
  `Pass::compute_only()` — a pass with no attachment usages whose record() runs
  OUTSIDE any rendering group, at its toposorted position, with barriers derived
  from its declared usages (image usages transition through the tracker; buffer
  usages merge into one flushed barrier). The post chain is the first customer:
  it lands between the last MSAA group's resolve into COLOR_TARGET and the
  composite group. New Access categories `StorageImageRead/Write` (GENERAL
  layout; the write scope also covers sampled reads because post compute
  samples the target it storage-writes back).
- **`sandbox/passes/post_pass.{hpp,cpp}` + `sandbox/shaders/post.slang`**
  (PostProcessPass, `debug_name "post"`, last in the demo/lookdev plans):
  histogram + exposure reduce + bloom pyramid + bloom apply (in place, into
  color_attachment_ — which gained STORAGE usage — so the composite AND the
  capture writer see the final HDR frame). Outline/rim SLOT: a documented no-op
  hook (`record_outline_slot`) at the end of the chain; depth (hz.depth) +
  depth-reconstructed normals (gtao.slang shows the reconstruction) available.
- **Bloom (tight & punchy, threshold-free):** 13-tap CoD downsample with
  KARIS AVERAGE + firefly clamp on the first (half-res) mip, plain averages
  after; 3x3 tent upsample-accumulate. CVars: r.bloom.enabled (STRING_BLOOM),
  r.bloom.intensity (0.04), r.bloom.clamp (64 knits, pre-average), r.bloom.radius
  (0.85 per-level accumulate scale — lower = tighter), r.bloom.mips (6).
  Verified against the sun disc / bright sky / specular glints (no emissives
  until 08). The bloom pyramid is intra-pass transient state in permanent
  GENERAL layout (07 cubemap precedent) with local barriers.
- **Auto-exposure:** 256-bin log2-luminance histogram (domain 2^-10..2^9 knits)
  over the PRE-bloom resolved HDR — the histogram bins are the first REAL
  FrameScratch customer outside geometry (256 x uint region in the per-slot
  arena). Percentile-trimmed mean (drop darkest 50% / brightest 5%, CVars
  r.exposure.cut_low/high) -> per-slot host-visible readback ring -> CPU reads
  it frames_in_flight later, smooths EV100 with separate up/down rates
  (r.exposure.speed_up 3.0 / speed_down 1.5 per second) + clamps
  (r.exposure.min_ev 6 / max_ev 17) + r.exposure.comp, and publishes to
  CompositePass. r.exposure.auto (STRING_EXPOSURE_AUTO) default ON;
  r.exposure.ev100 remains the manual override / pre-warmup value. THE LOOKDEV
  SCENE PINS MANUAL EXPOSURE by default (it is the calibration reference;
  demo_scene sets STRING_EXPOSURE_AUTO=0 with overwrite=0 so an explicit env
  still wins). Determinism note: the exponential adaptation snaps to target
  within a millistop — without the snap, two runs whose streaming warmup timing
  differed kept a permanent last-ulp EV difference (1-LSB full-frame diff).
- **GTAO (Jimenez et al. 2016), half-res, bent normals** (`gtao.slang`,
  recorded at the top of GeometryPass::record_compute): the depth source is
  the PREVIOUS frame slot's min-resolved hz.depth (04d chain REUSED — no
  dedicated resolve) under that frame's matrices; lit fragments reproject
  world positions through SceneData.prev_view_proj. Exact for static geometry
  under camera motion; disocclusions read stale AO for one frame and clamp to
  vis=1 off screen; ZERO temporal accumulation (cannot ghost — the no-TAA
  constraint). Why previous-frame depth: this is a forward renderer with no
  depth prepass — same-frame depth does not exist before shading, and a
  depth-only prepass would blow the 1.5 ms post budget on its own.
  3 slices x 5 steps/side, IGN spatial rotation (no temporal jitter), depth-
  reconstructed normals (outline-slot preference), distance-falloff horizons,
  cosine-weighted visibility + bent normal from the unoccluded arc midpoint,
  5x5 depth-aware spatial denoise. Output RGBA8: rgb = WORLD bent normal,
  a = visibility. Consumption (lighting.slang): diffuse ambient x multi-bounce
  visibility (Jimenez cubic fit vs the real albedo); specular ambient x bent-
  normal cone vs GGX reflection-lobe cone intersection (REPLACES the brief-07
  AO-derived approximation for the screen-space term; the baked-material AO
  keeps its 07 rules). CVars: r.gtao.enabled (STRING_GTAO, default on),
  r.gtao.strength (1.0), r.gtao.radius (0.8 m). Auto-off when the two-phase
  HiZ depth is unavailable (STRING_HIZ=0, warmup, resize). SceneData grew an
  APPENDED tail (prev_view_proj @704, gtao_slot @768, gtao_strength @772,
  sizeof 784; lighting_data.hpp asserts).
- **ACES 2.0 output transform + grading, baked into ONE LUT**
  (`string-engine/{include/string/core,src/core}/tonemap.*`, CompositePass):
  CPU bake at init (~0.4 s multithreaded, logged) into a 48^3 LUT stored as a
  2304x48 RGBA16F 2D strip; composite = exposure + ONE manual-trilinear LUT
  sample (two bilinear fetches + lerp). **LUT input-domain encoding: log2
  shaper over post-exposure values, enc = (log2(c) - (-12)) / 19, domain
  [2^-12, 2^7]** (post-exposure mid grey 0.18; a linear-domain LUT bands in the
  darks — constants locked in tonemap.hpp + composite.slang). r.tonemap =
  aces2 | aces1 (STRING_TONEMAP; aces1 = the v1 Narkowicz fitted curve for
  A/B, ALSO through the LUT so the composite has one path);
  r.tonemap.lut_size (48). Grading (HDR working space, composed into the same
  bake, re-bake on CVar change behind a logged device drain): r.grade.exposure
  / contrast (log, 0.18 pivot) / saturation / temperature + tint (CAT16 von
  Kries, luma-renormalized) / lift + gamma + gain (scalar v1, applied in
  shaper space). Neutral defaults. The CAPTURE WRITER now encodes per-PIXEL
  through the same exposure + CPU-side LUT trilinear (the aces2 DRT mixes
  channels; per-channel encode would lie).
- **HONEST DEVIATION — the "aces2" curve:** an ACES-2.0-STYLE CAM DRT —
  Hellwig2022/CIECAM16 JMh appearance model (dim surround, L_A=100, Y_b=20),
  the ACES 2.0 Daniele-Evo tonescale with the reference SDR-100-nit
  parameterization, hue-preserving chroma compression (M tracks the J rescale,
  exponent 0.9 — an approximation of the reference's chroma compression), and
  a bisection gamut mapper to Rec.709 along constant (J,h). Hue skews near
  clip are gone BY CONSTRUCTION (nothing operates per-RGB-channel), which is
  the user's stated motivation — but this is NOT bit-matched against the
  official ACES 2.0 CTL reference (cusp/reach tables and the exact chroma
  compression differ). Follow-up filed for the style checkpoint: validate
  against the reference implementation's sweep images before locking the look.
- Composite gained real update() work (LUT bake/rebake) — the renderer now
  calls composite_pass_.update() alongside the scene passes.

### Gate results (2026-07-23, pinned stores, sequential captures)

- **LUT identity sanity (numeric): PASS.** Neutralized chain (STRING_TONEMAP=
  aces1, STRING_EXPOSURE_AUTO=0, STRING_BLOOM=0, STRING_GTAO=0) vs the
  committed pre-09 HEAD binary (28e8b3f), interior + exterior 800x800:
  **max delta = EXACTLY 1 eight-bit step (PAE 1/255)**; 64% / 78% of pixels
  differ by that 1 LSB (48^3-LUT quantization of the analytic curve — raise
  r.tonemap.lut_size for tighter parity).
- **Histogram sums to pixel count: PASS.** dbg.exposure_verify: total 640000 =
  800x800, kept 288000 = exactly (1 - 0.50 - 0.05) x total.
- **Run-twice determinism (full chain, defaults, STRING_FIXED_DT): PASS.**
  interior / exterior / lookdev 800x800 AE=0 (after the EV snap fix above;
  the interior initially held a permanent 1-LSB full-frame echo of the
  pre-existing streaming-warmup timing through the exposure feedback loop).

## Fix session (2026-07-24) — four user-reported defects from live screenshots

User reviewed the live build and reported four defects. Root cause + fix per
defect below. NOTE: this session made code fixes only and did NOT run headless
captures — the user verifies live (their explicit direction: faster to eyeball
the real scene than to cross-reference captures). Build is green (`nix build
.#demo`). Each fix is behind an existing or new CVar for live A/B.

### Defect 1 — horizontal banding on the Sponza vault ceiling + lookdev ground
- **First attempt (falloff-range fix) was NOT sufficient — user disconfirmed
  live 2026-07-24** ("still banding; gtao strength 0 looks correct", which pins
  the defect inside the GTAO pass output itself). The falloff change
  (`atten = smoothstep(0,1, dist/radius)` replacing a form that never reached
  full attenuation inside the sampled range) is kept — it is correct on its own
  terms — but it was not the banding source.
- **Root cause, second pass: reconstruction xy/depth mismatch under the NEAREST
  depth sampler.** hz.depth is bound with a NEAREST sampler
  (geometry_pass.cpp hiz_sampler_); the half-res GTAO uvs
  (`(id+0.5)/half_size`) land exactly BETWEEN four full-res texels, so every
  `view_pos(uv, depth_at(uv))` paired an un-snapped xy ray with the depth of
  whichever texel NEAREST happened to snap to — a systematic half-texel
  mismatch with screen-periodic parity. At grazing (lookdev ground) and on
  smooth curves (vault) the per-texel depth gradient is large, so the center
  position, the normal-reconstruction taps AND every horizon sample carried
  correlated errors -> banded visibility. On convex geometry (sphere tops) the
  same error let near-tangent neighbours flip into phantom occluders, which
  also fed defect 3 through low vis / corrupt bent normals.
- **Fixes (gtao.slang):**
  1. `snap_uv()`: all reconstruction uvs (center, normal taps, horizon samples)
     snap to the full-res texel center the sampler actually reads, so xy and
     depth always describe the same surface point.
  2. **Tangent-plane angular bias** (standard HBAO/XeGTAO bias, ~6 deg): a
     sample only registers as an occluder if `dot(delta, N) >= 0.1 * dist`.
     Coplanar flat-ground neighbours and below-tangent convex neighbours
     (curvature is not occlusion) can then never fake a horizon regardless of
     residual reconstruction noise.
- A/B: `r.gtao 0` / `r.gtao.strength`. **NOT YET USER-VERIFIED.**

### Defect 2 — furnace test renders super dark (was correct flat white pre-09)
- **Root cause: two compounding.** (a) The furnace scene was being auto-metered
  to middle grey (a uniform-white scene must be PINNED to the calibrated EV, not
  metered). (b) GTAO ran on the depthful furnace scene, denting the uniform
  ambient (the "GTAO on the furnace must be 1.0" gate).
- **Fix:** (a) auto-exposure now defaults OFF (see defect 4), so furnace uses the
  manual calibrated `r.exposure.ev100` (14.6). (b) GTAO is force-disabled under
  `r.furnace` in `geometry_pass.cpp` (`gtao_allowed = enabled && !furnace_`), so
  ambient occlusion cannot perturb the uniform-white acceptance state.
- **Live round 7 (2026-07-24, screenshot): STILL WRONG — uniform GREY, not
  white. GTAO on/off no effect (so the uniformity/energy conservation is
  fine); auto-exposure = slightly brighter grey.** The missed half of the fix:
  the furnace environment has radiance 1.0 in scene units and the composite
  multiplies by the EV100 exposure scale (~0.033 at sunny-16) before the
  tonemap -> uniform ~20% grey, always. The task spec's actual requirement —
  "the furnace CVar must PIN manual exposure at the calibrated EV" — had not
  been implemented. Fix: `CompositePass::set_exposure_override(ev100, active)`
  (engine-side static; beats BOTH auto and manual; capture writer + UI
  compensation see it too via exposure_scale()), driven from
  GeometryPass::update: `set_exposure_override(log2(1000/1.2) ~ 9.70,
  furnace_)` — exposure scale exactly 1.0, so the radiance-1 furnace hits the
  tonemap at 1.0 and renders flat white.
- **Live round 8 (2026-07-24, screenshot): uniform but NOT white — light
  grey.** The exposed-1.0 assumption was wrong: filmic-style output transforms
  map scene 1.0 to only ~80-85% display (the shoulder reserves headroom above
  reference white; the aces2 CAM DRT lands lower still) — "scene 1.0 in =
  white out" does not hold for any filmic tonemap. Fix: pin re-targeted to
  exposed = 4.0 (EV ~7.70, `log2(1000/(1.2*4))`) — two stops above reference
  white lands the flat field at display white (>= ~0.96 sRGB) through BOTH
  curves while staying on the shoulder (not hard clip), so non-uniformities —
  the gate's actual signal — stay visible. User also asked whether the
  furnace-scoped override masks a general exposure bug: it does not — the
  furnace gate tests BRDF energy conservation (UNIFORMITY), which passed even
  in the grey renders; absolute exposure calibration is gated separately
  (sunny-16 middle-grey on lookdev, the LUT identity check, the TOD sweep). A
  synthetic radiance-1 field has no photographically "correct" EV; white is
  the test's canonical presentation and exists only by pinning.
  **NOT YET USER-VERIFIED.**
- NOTE (accepted workflow behavior, visible in furnace if looked for): at
  PARTIAL metallic (0 < m < 1) the metallic-workflow interpolation is
  inherently slightly lossy (F0 = lerp(0.04, albedo, m) with diffuse scaled by
  1-m does not sum to unit albedo), so partially-metallic trims may read
  faintly darker than the field. m = 0 and m = 1 are exactly energy-conserving
  (checked algebraically against the Fdez-Aguera closure in lighting.slang).
- **Live round 9 (2026-07-24): furnace USER-CONFIRMED flat white. Defect 2
  CLOSED.**

### Defect 4 outcome — CLOSED (live round 9, 2026-07-24)
- User verdict: auto-exposure sweep gives a very consistent level; pumping
  "almost sub-perceptual"; **default OFF is the right call and stays** (user
  prefers manual; auto remains the r.exposure.auto opt-in with the
  scene-referred compensation curve as implemented).

### Residual (live round 9): VERY minor banding — arcades (GTAO) + skybox
- User spotted two very minor banding sources (likely pre-existing, newly
  noticeable once the gross banding was gone; the arcade banding vanishes with
  r.gtao 0, the sky banding is GTAO-independent). Two independent quantizers,
  same artifact class:
  1. **AO visibility stored in RGBA8**: 1/255 vis steps multiply into the
     ambient on smooth slowly-curving receivers (the vaults) as wide soft
     bands. Fix: GTAO raw + final targets RGBA8 -> **RGBA16F** (half-res,
     ~4 B/px extra; the bent normals gain precision for free).
  2. **No output dither**: the composite quantizes smooth gradients (the
     procedural sky) straight into the 8-bit sRGB swapchain. Fix: +-0.5
     ENCODED-LSB IGN dither in composite.slang, applied in the sRGB-encoded
     domain (encode, add noise/255, decode — the swapchain quantizes ENCODED
     values, so a linear-domain dither would be mis-scaled across the tonal
     range). SPATIAL noise only (no frame term): the no-TAA rule and the
     run-twice determinism gate hold (captures grab the pre-composite HDR
     target and are unaffected; the capture writer's CPU encode intentionally
     stays un-dithered, so screen vs capture may differ by 1 encoded LSB).
- **Live round 10 (2026-07-24):** sky banding GONE at the top, RESIDUAL in the
  dark sub-horizon gradient; arcade banding SURVIVED the 16F AO targets (so it
  is real structure in the GTAO signal, not storage quantization); dither
  accepted, user prefers real sources fixed at the source (correct instinct —
  the surviving bands prove the half-LSB dither hides nothing bigger than
  half a step). Sub-horizon fix: **r.tonemap.lut_size default 48 -> 64** —
  the trilinear LUT's piecewise-linear segment kinks reach a couple of
  encoded LSBs exactly where the transform bends hardest (dark gradients),
  beyond the dither's reach. Arcade banding: root cause not yet isolated; two
  candidates queued for a user A/B (spec-occ smoothstep contours vs
  horizon-sampling texel quantization on the concave vault) —
  r.gtao.spec_occ 0 splits them.
- **Live round 11 (2026-07-24) — user A/B results + three fixes:**
  1. **Sub-horizon sky:** LUT size "affects it a little"; at the clamp
     ceiling 96 (user set 128, which clamps to 96) it is effectively
     sub-perceptual, especially under rapid TOD change. Fix: **default
     lut_size -> 96** + a **disk cache** for baked LUTs
     (user_cache_dir("tonemap"), keyed by curve + grading + size + format
     version) so the ~3 s 96^3 bake is paid once per parameter set;
     subsequent launches load in ms ("[tonemap] cache-loaded" log line).
  2. **Arcade banding — mechanism CONFIRMED by the user's A/B:** survives
     spec_occ 0 (it lives in the diffuse visibility; spec-occ only darkens
     it) and **scales strongly with r.gtao.radius** (soft at 0.4; big, dark,
     very perceptual at 1.6) = horizon-sampling discretization. The f*f
     center-clustered steps sampled the far field with a sparse
     screen-coherent pattern; on the CONCAVE vault (where distant samples are
     legitimate occluders) the horizon estimate jumped between sparse far
     samples as depth varied -> radius-scaled contour bands. Fix
     (gtao.slang): **uniform step spacing** (largest gap bounded at
     radius/kSteps) + **per-(slice, side) decorrelated step phase**
     (golden-ratio offsets) so residual discretization becomes per-pixel
     noise the 5x5 denoiser averages away.
  3. **Rough-metal drastic flip under spec-occ (user images 14/15):** the
     narrow-cone-at-R model over-occludes rough lobes (they are nearly
     hemispherical — the whole env term matters, not a cone around R). Fix
     (lighting.slang): blend the cone result toward the plain
     cosine-hemisphere visibility with roughness (Frostbite convention:
     spec-occ -> AO at roughness 1): `so = lerp(1 - occ, vis, roughness)`.
  **NOT YET USER-VERIFIED.**

### Defect 3 — dark upper hemispheres on smooth/mirror metals (lookdev)
- **Root cause: the bent-normal specular-occlusion cone formula treated surface
  CURVATURE as occlusion.** The old form used
  `a_s = lerp(0.1, pi/2, roughness^2*2)` (≈0.1 for a mirror) and
  `gtao_spec = 1 - smoothstep(|a_v - a_s|, a_v + a_s, d_bc)` where
  `d_bc = angle(bent, R)`. On a curved sphere top in the open, vis == 1 (nothing
  occludes) so the bent normal ≈ N, but R sweeps away from N with view angle, so
  `d_bc` grows and fell inside the ramp → phantom occlusion darkening the sky
  reflection. Curvature is not occlusion.
- **Fix:** rebuilt the cone intersection so an unoccluded point (vis → 1 ⇒
  aperture `a_v` → pi/2, the full open hemisphere) returns visibility 1 for ANY
  reflection direction in the front hemisphere:
  `cos(a_v) = 1 - vis`, `a_s = roughness * (pi/2) * 0.5`,
  `occ = smoothstep(a_v - a_s, a_v + a_s, d_bc)`, `gtao_spec = 1 - occ`.
  **User disconfirmed the first round live 2026-07-24** (spheres still dark with
  the new formula; strength 0 correct) — consistent with the GTAO pass itself
  producing low vis / corrupt bent normals on convex geometry, i.e. the
  defect-1 reconstruction bug. The defect-1 second-pass fixes (snap_uv +
  tangent bias) are expected to restore vis ~ 1 on open sphere tops, which this
  formula then passes through as full-strength reflection.
- **A/B lever added:** `r.gtao.spec_occ` (STRING_GTAO_SPEC_OCC, default on). Off
  falls back to brief-07's AO-derived Lagarde `spec_ao` only (SceneData
  `debug_flags` bit 2 gates the bent-normal path in `lighting.slang`).
- **Live round 2 (2026-07-24): banding perceptually GONE (user-confirmed) and
  sphere reflection interiors correct — but a PIXELATED BLACK RIM remains at
  the sphere silhouettes** (gone with `r.gtao.strength 0` or
  `r.gtao.spec_occ 0`). Root cause: the AO texture stores normalized bent
  vectors on surfaces but an exact ZERO vector on sky texels; the
  consumption-side bilinear sample shortens the decoded vector to near/exactly
  zero wherever its half-res footprint straddles a silhouette, and
  `normalize()` of that is garbage (NaN at exact zero) -> huge d_bc -> full
  phantom occlusion at half-res granularity (the pixelation). Fix
  (lighting.slang): decoded bent-vector LENGTH as confidence
  (`bn_conf = smoothstep(0.5, 0.9, len)` — ~1 on valid interior texels,
  collapsing at edges), `occ *= bn_conf` — an unreliable bent direction never
  occludes.
- **Live round 3 (2026-07-24): console fixed, NO regressions, but the rim
  SURVIVED the bn_conf fade** — disproving the shortened-vector theory as the
  rim's cause (the rim texels hold valid unit vectors). Actual root cause: the
  cone-intersection RAMP was centered on the cone edge. At every curved
  silhouette the incidence goes grazing, so `d_bc = angle(bent~N, R) -> pi/2`,
  while `a_v = acos(1-vis)` CAPS at pi/2 even for a fully open point — the
  centered smoothstep `[a_v - a_s, a_v + a_s]` therefore evaluated at its
  midpoint: **every unoccluded silhouette pixel got occ = 0.5**, at half-res
  granularity = the pixelated dark rim. Fixes (lighting.slang):
  1. **One-sided ramp** `smoothstep(a_v, a_v + max(a_s, 0.1), d_bc)` —
     `d_bc <= a_v` is exactly unoccluded; the lobe half below the geometric
     horizon at grazing is already handled by the BRDF/prefilter (counting it
     again double-darkens).
  2. **Open-hemisphere gate** `occ *= 1 - smoothstep(0.9, 1.0, vis)` — vis == 1
     means no occluder exists anywhere, so spec-occ vanishes identically
     (also guards against bent-normal wobble from the half-res denoise).
- **Live round 4 (2026-07-24): rim substantially reduced (one-sided ramp +
  gate confirmed as improvements), no interior/ground regressions — but dark
  pixelated edge artifacts REMAIN on sphere silhouettes when the sphere sits
  near the screen edge** (spec_occ-only; off = clean). Remaining root cause:
  the PLAIN BILINEAR upsample of the half-res AO. At silhouettes the 2x2
  footprint always blends this surface's texels with the BACKGROUND's; sky
  (zero vector) and fully-open background (vis = 1) were already defended, but
  background texels carrying REAL occlusion (e.g. ground contact shadow behind
  the sphere edge) hold valid unit bent normals pointing elsewhere — the
  leaked cone reads as occlusion. Off-axis viewing at the screen periphery
  widens the grazing silhouette band, making it most visible there. Fix:
  **normal-aware joint upsample** (lighting.slang) — the four half-res texels
  are weighted by bilinear coverage x `pow(saturate(dot(bent_i, geom_n)), 4)`
  (un-normalized dot, so zero-length sky texels self-reject); cross-surface
  texels are rejected instead of blended, for BOTH the diffuse vis and the
  spec-occ terms. Plumbing: SceneData gained `gtao_w/gtao_h` in the two spare
  tail pad slots (offsets 776/780; sizeof and all earlier offsets unchanged,
  lighting_data.hpp asserts extended).
- **Live round 5 (2026-07-24, with screenshot): edge artifact reduced but
  still present.** The joint upsample carried a flawed FALLBACK: when NO texel
  agreed with the fragment's surface (silhouette fragments can sit entirely
  over background AO — the reverse-Z MIN depth resolve erodes silhouettes by
  up to a pixel), the `+1e-3` epsilon weight degraded to plain-bilinear
  averaging of the DISAGREEING background texels — recreating exactly the leak
  the upsample was built to prevent. Fix: epsilon removed; `g_wsum <= 1e-3`
  now skips GTAO for the fragment entirely (vis = 1, no spec-occ) — the same
  no-data rule disocclusions use. Never borrow the background's occlusion.
- **Live round 6 (2026-07-24): "same behavior" — the no-data fallback changed
  NOTHING**, which rules out background leakage entirely: the artifact texels
  are the sphere's OWN near-silhouette texels, with agreeing normals and
  genuinely low vis. Analysis: for a fully open surface at GRAZING (N _|_ V —
  exactly the silhouette case — gamma -> pi/2 in ALL slices), the 3-slice
  estimator's worst IGN phase yields **vis ~ 0.9 instead of 1.0** (the sum of
  per-slice projected-normal weights |cos(delta_i)| spans ~1.73..1.97 around
  the expected 1.91; the arc estimator is exact only in expectation over slice
  directions), and the 5x5 denoise cannot average the phases out at a
  silhouette because the depth-valid neighborhood there is a thin band.
  vis 0.9 -> a_v ~ 84.6 deg vs d_bc ~ 90 deg: a few-degree deficit inside a
  ~6 deg ramp -> occ up to ~0.7, IGN-phase-varying per pixel = the pixelated
  band; oblique screen-edge rays widen the grazing band = worse at screen
  edges. Fix: **calibrate the term to the estimator's noise floor** — minimum
  occlusion-onset ramp widened 0.1 -> 0.35 rad (~20 deg) and the
  full-visibility gate widened from smoothstep(0.9, 1.0, vis) to
  smoothstep(0.8, 0.95, vis). Real occluders (vis 0.3..0.7, d_bc tens of
  degrees past a_v) sit outside both margins and keep the full term. If an
  artifact still shows after this round, the fallback plan is
  r.gtao.spec_occ default -> OFF (brief-07 AO-derived Lagarde) with the bent
  path kept as an explicit opt-in for future refinement.
- **Live round 7 (2026-07-24): USER-CONFIRMED — edge artifacts GONE with the
  noise-floor calibration.** User screenshots: Sponza arcades (vaults smooth,
  banding-free, subtle AO in the vault intersections/column bases) + mirror
  ball near the ground (clean silhouette, bright sky top, ground reflection
  below). Defects 1 and 3 are visually RESOLVED. Whether the bent-normal path
  earns its keep vs r.gtao.spec_occ=0 (contact-region reflection darkening is
  the visible payoff) is left with the user via the live A/B. Remaining to
  verify: furnace flat-white (defect 2), manual-default + opt-in
  auto-exposure TOD behavior (defect 4).

### Defect 5 (new, live round 2) — debug console renders super dark
- **Root cause:** the UI (console/HUD shapes + SDF text) draws DISPLAY-referred
  colors into the SCENE-referred HDR target inside the MSAA group chain; the
  composite then multiplies the whole frame by the EV100 exposure scale
  (1000/(1.2*2^EV) ~ 0.033 at the sunny-16 default) before the tonemap LUT —
  authored-white UI lands at ~0.03 scene units -> dark grey. Auto-exposure
  previously masked this in metered scenes (interior EVs ~9-10 give a scale
  near 1); with manual EV 14.6 now the default (defect 4) the attenuation is
  always visible. GTAO is unrelated (matches the user's on/off observation).
- **Fix:** pre-divide UI colors by the SAME frame's exposure scale
  (`inv_exposure = 1 / CompositePass::exposure_scale()`, new push-constant
  field in ui_shader.slang + text_shader.slang, multiplied into
  fill/stroke/glyph rgb in ui_pass.cpp) so the composite's exposure cancels
  exactly on UI pixels — UI brightness becomes independent of scene EV. Whites
  then hit the tonemap at ~1.0 (-> ~0.9 display), the intended SDR-white look.
- **Known residual (accepted for now):** the UI still lives in the HDR frame
  BEFORE the post chain (it renders inside the MSAA group chain; the post pass
  needs the post-resolve target, so plan reordering cannot lift it out), so
  bloom sees the compensated (~30 knit) UI and may add a soft glow around
  bright panels/text, and the histogram meters it when auto-exposure is opted
  in. Moving the UI post-composite (a true display-referred overlay) remains
  the documented open follow-up. **NOT YET USER-VERIFIED.**

### Defect 4 — auto-exposure too aggressive; night exposed to daytime
- **(a) DEFAULT auto-exposure OFF.** `r.exposure.auto` now defaults `false`
  (`composite_pass.cpp`); manual `r.exposure.ev100` is the default path again.
  `r.exposure.auto=1` opts in. (The lookdev scene's explicit pin is now
  redundant but harmless.)
- **(b) Scene-referred exposure compensation curve** (Frostbite/COD "Moving to
  PBR", Lagarde & de Rousiers, exposure-comp curve) for when auto IS enabled, in
  `post_pass.cpp::update()`. Instead of always metering the average to middle
  grey (which pumps night up to daytime), the target EV is a piecewise-linear
  function of the MEASURED scene EV about the reference key `key_ev = 14.6`:
    - measured < key_lo (key-3): slope 0.5 — dark scenes keep half their
      underexposure ⇒ **night stays dark**;
    - key_lo..key_hi (key+2): slope 1 — full grey-world metering in the mid band;
    - measured > key_hi: slope 0.5 — bright/daylight compressed, no blow-out.
  The curve is continuous at both knees. Min/max EV clamps
  (`r.exposure.min_ev` 6 / `max_ev` 17) still bracket the TOD range and are
  applied after the curve. TOD sweep expectation: readable at noon, DIM at
  night, no pumping.

### NEEDS VISUAL VERIFY (user, live)
1. Sponza vault ceiling + lookdev ground: horizontal banding GONE (toggle
   `r.gtao 0` to confirm GTAO was the source; the vault should still have a
   subtle ambient dent, just no stripes).
2. `r.furnace 1` (STRING_FURNACE=1): scene disappears into FLAT WHITE again at
   every roughness/metallic — no dark AO dent, no grey auto-metering.
3. lookdev mirror-row spheres: bright zenith sky reflection on the smooth/low-
   roughness metal tops (compare `r.gtao.spec_occ 1` vs `0` — both should now
   read bright on the open sphere tops; occluded crevices between spheres may
   still darken, which is correct).
4. Default launch = MANUAL exposure (night is dark by default). Then
   `r.exposure.auto 1` + TOD sweep (STRING_TOD / T key): noon readable, night
   stays dim, no exposure pumping across the sweep.
