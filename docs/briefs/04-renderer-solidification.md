# Brief 04 — Renderer solidification: transparency, cutout, pipeline debt

Status: COMPLETE. M1 (cutout + two-sided), M2 (sorted transparency), M3 (index-heap removal, ~43 MiB reclaimed), M4 (per-cascade shadow draw-cull) all LANDED. Validation clean, flake check green, captures deterministic at settled frames. NEEDS VISUAL VERIFY on M1 (Sponza cracks gone) + M2 (blended-quad sort) — see agent report.

## Goal

Harden the single GPU-driven geometry path before anything is built on top of
it. Two halves: (a) the **transparency/cutout/two-sided** work originally
bundled into the VFX brief — it is core pipeline work, not VFX (it fixes the
Sponza thin-black-cracks bug and it touches the draw-cull compute + command
lists); (b) the **parked debt** from brief 03 recorded at its close.

## Context

Brief 03 (+03b/03c) left exactly one geometry path: draw-cull compute →
5 `vkCmdDrawMeshTasksIndirectCountEXT`/frame → task (frustum/cone/HiZ) → mesh →
shared `lighting.slang`. Everything opaque. glTF alphaMode/doubleSided are
currently ignored, which is why Sponza's curtains/foliage/decal geometry show
black cracks and holes.

## Decisions (locked with user, 2026-07-22 reorder)

- **Alpha-tested cutout**: honored in the main pass, the depth prepass, AND the
  shadow pass (foliage must shadow correctly). Standard approach: a cutout
  material flag in `GpuDrawInfo` + `clip()` in the fragment/shadow shaders
  sampling base-color alpha. Prepass HiZ stays conservative with cutout (a
  cutout-written depth is fine; do not skip cutout in the prepass or HiZ will
  over-cull what's behind it).
- **Two-sided materials**: per-draw flag; standard solution is disabling
  backface *rasterizer* cull for those draws — but with meshlets the cone cull
  in the task shader must ALSO be bypassed for two-sided draws (a backface cone
  reject on a two-sided meshlet is a correctness bug). Since dynamic rasterizer
  state per-draw doesn't exist inside one indirect draw, split the command
  lists: opaque one-sided vs two-sided get separate indirect draws (still O(1)
  draw calls; 5 → ~7/frame). Flip the normal toward the viewer in the fragment
  shader (`SV_IsFrontFace`) so lighting is correct on back faces.
- **Sorted alpha-blend transparency pass**: a separate forward pass after
  opaque + sky, depth-tested against opaque depth but not depth-written.
  Per-draw back-to-front CPU sort v1 (transparent draw counts are small;
  per-meshlet/per-triangle sorting is explicitly out of scope). Reuses the
  meshlet task/mesh path minus HiZ (occlusion culling translucents against
  opaque depth is fine and stays). No OIT — deferred until content proves need.
- **glTF plumbing**: loader reads `alphaMode` (OPAQUE/MASK/BLEND), `alphaCutoff`,
  `doubleSided` and routes draws to the right list at flatten time.
- **Parked debt from brief 03** (in scope, in this order of value):
  1. **GPU index heap dead weight (~45MB)**: the meshlet path never reads the
     index heap; refactor `geometry_streamer` to stop allocating/uploading GPU
     index memory (CPU-side indices stay — meshlet_builder consumes them).
  2. **Per-cascade shadow draw-level cull**: the draw-cull compute currently
     emits a resident-only shadow list; add a per-cascade light-frustum
     draw-level reject (the `mode` seam reserved in `meshlet_draw_cull.slang`).
     Must keep off-camera casters that still cast into view — cull against the
     CASCADE's frustum, never the camera's.
- The interior HiZ 1.27% residual is accepted (user), NOT in scope.

## Milestones

1. Cutout + two-sided (loader flags → split lists → shaders). Sponza cracks
   gone; foliage/curtain shadows correct.
2. Sorted transparency pass; verify against a synthetic blended-mesh scene
   (Bistro glass arrives later, in brief 10).
3. Index-heap refactor; report VRAM reclaimed; streaming still solid
   (`STRING_MESHLET_VALIDATE`, capture parity vs baseline).
4. Per-cascade shadow draw cull; report shadow-pass meshlet stats before/after;
   frozen-frustum (F) behavior stays correct.

## Acceptance

- Sponza interior/exterior captures: no black cracks; numeric diff vs baseline
  isolates the intentional fixes (changed pixels are AT the former cracks).
- Opaque-only scenes remain byte-identical after milestones 3–4 (parity gate,
  same discipline as 03c).
- [frametime] + [mesh-cull] numbers reported per milestone; validation clean;
  flake check green. NEEDS VISUAL VERIFY at milestones 1 and 2.
