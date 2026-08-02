# Brief 04f — Cooked format v3: vertex quantization

Status: not started. Runs after 04e (or opportunistically between briefs —
it is independent of the graph work but touches the mesh-shader vertex pull,
so do not overlap it with 04c/04d agents).

## Goal

Halve the vertex footprint before content accumulates: cooked format v3
quantizes the 48B `String::Vertex` stream. Formats are cheapest to change
NOW — this is format-locking work that gets strictly more expensive with
every asset added.

## Decisions (locked with user, 2026-07-22)

- **Quantized vertex (target 24B, verify exact packing against SPIR-V):**
  - Position: 3x 16-bit, normalized to a per-draw (per-chunk) AABB stored in
    the draw table (dequantize = fma with per-draw scale/bias). The bake
    already computes tight per-draw bounds; chunking (04b) keeps ranges
    small so 16 bits is plenty of precision.
  - Normal + tangent: octahedral-encoded, 2x 16-bit each (snorm16 oct);
    tangent sign in the spare bit budget. Replaces the 10:10:10:2 tangent +
    12B normal.
  - UV: 2x 16-bit half (evaluate range on Sponza/Bistro-class UVs; fall back
    to unorm16 with per-draw scale/bias if halves show precision issues on
    tiled UVs).
  - Color: drop if unused by content (audit — the loader carries it; if only
    the transp-test quads use it, move that to a material constant), else
    RGBA8.
- **Dequantization happens in ONE shared Slang function** (meshlet.slang or
  the cull module's sibling) used by the mesh shader, shadow mesh shader,
  and any future vertex-pull backend — single definition, no drift.
- **Bake-side**: quantization in bake_scene (procgen inherits it); cooked
  format version bump + manifest hash change forces re-cook; determinism
  test still byte-identical (quantization is pure math — no float
  nondeterminism sources; round-to-nearest-even, document it).
- **Streamer**: element size changes ripple through heap sizing/suballoc;
  CPU-side debug accessors (cpu_vertex) dequantize for validators.
- **Error budget**: STRING_MESHLET_VALIDATE extended with a quantization
  error check (max position error vs float source < half a texel at
  reference viewing distance; report the number). Visual acceptance is
  capture-diff based: small AE is EXPECTED (sub-texel vertex movement);
  gate on eyeballed capture pairs + no cracks between adjacent draws
  sharing edges (quantization is per-draw — verify chunk seams stay
  watertight: adjacent chunks quantize the same source position against
  DIFFERENT AABBs, so seam verts may diverge by up to one quantum each.
  If cracks appear, the fix is snapping chunk AABBs to a shared world-space
  quantization grid at bake time — implement that from the start if Sponza
  shows any seam artifacts).

## Milestones

1. Format v3 write/read + shared dequant function + mesh/shadow shader
   switch; re-cook; captures + seam inspection; validator numbers.
2. Streamer/heap ripple + VRAM and cooked-file-size numbers (expect ~half:
   ~150MB main pack -> ~75-90MB).
3. Close-out: docs, brief 04b format section updated to v3.

## Acceptance

- Determinism test green; validator error budget met and reported; no seam
  cracks (eyeball + capture zoom); VRAM + disk numbers reported; flake
  check + demo green; validation clean.
- NEEDS VISUAL VERIFY: interior/exterior stills + flythrough looking for
  shimmer/cracks at chunk seams and silhouette wobble (quantization
  artifacts are geometric and visible in motion).
