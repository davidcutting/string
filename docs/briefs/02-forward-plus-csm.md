# Brief 02 — Froxel Forward+ lighting + cascaded shadow maps

Status: DONE + visually verified 2026-07-21 (three fix rounds: SceneData natural-layout offsets, SV_VertexID/SV_InstanceID base restoration — see memory/roadmap for the gotcha list)

## Goal

Many dynamic local lights (spell density) via clustered Forward+, and a
seamless-world-ready cascaded shadow map with a continuously moving sun (full
dynamic time-of-day). The existing Cook-Torrance BRDF, normal-offset shadow
bias, and sky-derived ambient all carry over.

## Current state

- One directional sun + hemispheric/sky ambient in `3d_shader.frag`
  (`GeometryPush` carries sun/ambient); PBR is Cook-Torrance GGX.
- Single 2048² shadow map, ortho fit to whole scene AABB, rendered inside
  `GeometryPass::record_compute`, 3×3 PCF + normal-offset bias (values tuned,
  keep them).
- No local lights exist anywhere.

## Decisions (locked with user)

- **3D froxels** (screen tiles × depth slices; start 16×16px × 24 log-depth
  slices, dimensions as CVar-able constants). Compute pass bins point+spot
  lights into per-froxel lists; fragment shading reads its froxel's list.
- **Light types v1: point + spot**, emissive already exists. Target: hundreds
  of dynamic lights. Local-light *shadows* are out of scope (budgeted, later).
- **CSM: 3 stabilized cascades at 2048²** default — texel snapping mandatory
  (moving sun + moving camera must not shimmer). Cascade count and resolution
  are **quality-tier CVars from day one**.
- **Shadow edge: crisp tight PCF** (5×5 max) — stylized art reads best crisp.
  No PCSS/contact-hardening.
- Sun direction animates (time-of-day); nothing may assume a static sun
  (the current once-computed `light_view_proj_` becomes per-frame).

## Correctness fixes folded in (user-verified artifacts, 2026-07-21 screenshots)

This brief rewrites `3d_shader`/`shadow` anyway (including their Slang port — watch
the known Slang gotchas: entry-point naming, column-major matrices, DrawParameters);
these known artifacts MUST be fixed as part of that rewrite, not deferred again:

- **Ambient/IBL occlusion.** Today `ao = 1` everywhere: every surface receives the
  full sky dome + warm ground-bounce regardless of enclosure — undersides of vaults
  and pillar capitals glow warm with no physical light path. Fix: sample the glTF
  **occlusion texture** (Sponza ships them; add the slot alongside base/normal/MR)
  and attenuate BOTH ambient diffuse and ambient specular (specular-occlusion
  approximation from AO is fine). Froxel local lights must not reintroduce the
  problem (they're direct light — occlusion for them comes from shadows, not AO).
  Scope note (settled with user): material AO only covers assets that ship it —
  the generic layer for procgen/unbaked geometry is SSAO (brief 05); the two
  multiply, per standard practice. Baked AO remains correct under the moving sun
  because it attenuates only the directionless ambient term; direct light gets
  its occlusion from the (fully dynamic) shadow maps.
- **Real vertex tangents (replaces derivative TBN — user decision 2026-07-21).**
  The screen-space-derivative tangent frame is a shortcut and it shows (noisy at
  grazing → speckled highlights). Do it the standard way: use the glTF `TANGENT`
  attribute when present, **generate via MikkTSpace at load when absent** (the
  reference lib; this keeps it generic for any asset incl. future procgen). Vertex
  gains a packed tangent+sign (~4B, e.g. 10:10:10:2 or 4×snorm8) — grow the locked
  44B layout deliberately: update the `static_assert`s, the Slang/GLSL struct, and
  the flatten path together. PLUS **geometric specular anti-aliasing** (widen
  roughness by normal variance, Kaplanyan-style) — complementary, not a substitute:
  tangents fix the frame, specular AA fixes highlight shimmer in motion.
- **Grazing-angle shadow speckle.** Surfaces nearly parallel to the sun flicker
  between lit/shadowed (sparse bright dots inside shade). Re-tune slope-scaled +
  normal-offset bias for the new CSM (per-cascade texel size changes the constants);
  verify specifically on sun-grazing vaults/ceilings.

## Milestones

1. **Light scene data**: lights as a sandbox-side array (pos/dir, color,
   intensity, radius/cone) in an SSBO; a synthetic **light stress scene**
   (hundreds of moving colored lights over Sponza) as the test bed.
2. **Froxel culling compute + shaded fragment path**: froxel grid build, light
   binning, `3d_shader.frag` accumulates froxel lights with the existing BRDF.
   Debug view: froxel light-count heatmap (CVar toggle).
3. **CSM**: split-scheme (practical split), per-cascade stabilized ortho fit,
   cascade selection + blend band in the fragment shader, quality CVars.
   Shadow draw list must include streamed geometry (fix the current static
   `shadow_indirect_buffer_`).
4. **Dynamic sun**: time-of-day CVar/debug slider driving sun direction +
   sky/ambient (already sky-derived, should follow automatically); verify no
   cascade shimmer while sun and camera both move.

## Acceptance

- Stress scene: hundreds of moving lights, frame budget held (report numbers
  via the profiler/log), froxel heatmap sane (no gross over-binning).
- No light pops at froxel/cascade boundaries; no shadow shimmer under
  sun+camera motion; acne/peter-panning no worse than current single map.
- Validation clean; `nix flake check` green.
- NEEDS VISUAL VERIFY: heatmap, cascade blends, moving-sun stability.
