# Brief 08 — VFX: GPU particles, telegraphs, decals

Status: not started

(Moved from slot 04 in the 2026-07-22 reorder. The transparency/cutout/
two-sided pass moved OUT to brief 04 (renderer solidification) — this brief
assumes it exists. The UI widget kit (05), debug tooling (06), and PBR/HDR
lighting (07) also precede this brief now: effects are authored with HDR
emissive values from day one (bloom in 09 consumes them), and the overdraw
budget is measured with brief 06's per-pass GPU timings.)

## Goal

The combat-readability pillar (WildStar × PoE): GPU-simulated particles,
**terrain-conforming ground telegraphs as a first-class system**, and decals —
all data-driven and hot-reloadable, with an explicit **overdraw budget**
measured from the first effect.

## Decisions (locked with user)

- **Particles**: compute-simulated (spawn/update/compact), indirect-drawn soft
  billboards; additive + alpha-blend modes; trails/ribbons; mesh emitters.
  Sorted back-to-front within brief 04's transparency pass (per-system sort
  v1). Emissive colors are HDR values in brief 07's unit system.
- **Telegraphs: terrain-conforming projection** (depth-reconstruction decal
  style — shapes hug slopes/stairs/props). Shape set: circle, cone, line/rect,
  donut; states: warning fill (grows/animates) + imminent; batched in one pass;
  driven by a plain data interface (position, shape, params, timings) that
  server combat data will feed in Phase B — sandbox feeds it synthetically now.
  Ground decals (targeting circles, impact marks) share this projection path.
- **Authoring: in-engine effect editor panel** (user choice): declarative
  hot-reloadable effect files are the source of truth (via brief 01's generic
  watcher); the editor panel is built on the brief-05 widget kit (numeric
  fields/sliders, curve editor, color picker), save-to-file. Curves are numeric
  keyframe lists in the file format.
- **Overdraw budget is part of the deliverable**: an overdraw heatmap debug
  view and a per-frame transparency-cost stat (surfaced as brief-06 CVars/HUD
  entries); a synthetic **PoE-density stress scene** (dozens of simultaneous
  effects + telegraphs) with per-pass GPU timings reported. Soft particles need
  depth — coordinate with the depth layout established in brief 03.

## Milestones

1. Particle sim + billboard draw + effect file format + hot reload.
2. Telegraph projection pass + shape/state set + synthetic combat demo.
3. Trails/ribbons + mesh emitters + soft particles.
4. Effect editor panel; overdraw heatmap + stress scene + numbers.

## Acceptance

- Stress scene holds frame budget with overdraw numbers reported; heatmap sane.
- Telegraphs conform on slopes/stairs/props with crisp edges; states animate.
- Effects edit live via file save AND panel; validation clean; flake check green.
- NEEDS VISUAL VERIFY at milestones 2 and 4 (user judges readability + feel).
