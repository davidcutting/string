# Phase A implementation briefs

Each file is a self-contained brief for an Opus implementation agent, in execution
order. Decisions in these briefs were made with the user (2026-07-21 walkthrough);
agents implement them, they do not re-decide them. Parent spec:
`docs/engine-feature-spec.md`.

## Order

Reordered with the user 2026-07-22 (after brief 03 closed): solidify the
pipeline and land UI + tooling + PBR *before* VFX/post — the last three briefs
proved tooling leverage, the UI was accreted placeholder code, and post/VFX
want the HDR/PBR foundation under them.

Extended again 2026-07-25 (after 09b closed): a five-brief tooling/UI arc (`11`–`15`)
inserted before 08 VFX — render-pipeline modularity, a full docking panel system, a matured
widget set, and an in-engine render debug UI (inspect + manipulate). Phase A exit needs the
pipeline modular, the debug experience real, and the UI more mature than brief 05 delivered.

1. `01-slang-hot-reload.md` — Slang + reflection-driven layouts + hot reload — **DONE**
2. `02-forward-plus-csm.md` — Froxel Forward+ lighting + cascaded shadows — **DONE**
3. `03-mesh-shaders.md` — GPU-driven meshlet pipeline + HiZ occlusion + LOD — **DONE**
4. `04-renderer-solidification.md` — Transparency/cutout/two-sided + brief-03 debt — **DONE** (visual verify passed 2026-07-22)
5. `04b-asset-pipeline.md` — Bake library + cooked scene format + spatial chunking — **DONE** (chunking default pending 04c)
6. `04c-global-meshlet-list.md` — Per-view meshlet worklists; kill per-draw dispatch fan-out
7. `04d-two-pass-occlusion.md` — Two-phase temporal occlusion + portable cull module (kills the double raster)
8. `04e-render-graph-maturation.md` — Graph-driven sync, aliasing, scratch, hardware-true queues
9. `04f-cooked-format-v3-quantization.md` — Quantized vertex stream (~half the geometry footprint)
10. `05-ui-maturation.md` — UI abstraction hardening + feel + gamepad — **DONE** (ran parallel to 04c, 2026-07-22; solidification still ends at 04f)
11. `06-debug-tooling.md` — CVars/console, per-pass GPU profiling, inspector, capture — **DONE** (three slices, 2026-07-22)
7. `07-pbr-ibl.md` — Dynamic sky IBL, physical light units, EV exposure — **DONE** (user-verified 2026-07-23)
8. `09-post-processing.md` — Bloom, auto-exposure, GTAO, ACES 2.0 LUT — **DONE** (pulled ahead of 08; defect marathon user-verified 2026-07-24; SMAA deferred)
9. `09b-probe-gi.md` — Relightable irradiance probe volume — **FUNCTIONALLY CLOSED** (2026-07-25; GI + CSM done, runtime cost deferred to the perf+quant brief; HW-RT backend = Phase 2)
10. `11-pipeline-modularity.md` — Decompose the GeometryPass monolith into registered passes; graph abstraction update (pass registration/metadata, dynamic enable/disable in the toposort, target tagging); introspectable+mutable light/material state; pass enable/disable proven live via CVar toggles *(not yet written — next up)*
11. `12-ui-panels.md` — Full ImGui-like dockable/splittable/tabbed panel system + layout persistence *(not yet written)*
12. `13-ui-widgets.md` — Widget set (sliders, drag-values, collapsibles, tabs, tables/trees, color) inside the panels; immediate-mode CPU cost *(not yet written)*
13. `14-debug-ui-inspect.md` — Debug tools in the UI: render-target visualizer, material/G-buffer channel isolation + material-preview sphere, per-pass toggles + inline timings, live CVar sliders *(not yet written)*
14. `15-debug-ui-manipulate.md` — Interactive debug: transform gizmos (point lights first, generic ray-pick + handle system) + live material-property editing for scene testing *(not yet written)*
15. `08-vfx-telegraphs.md` — GPU particles, telegraphs, decals
16. **End-of-Phase-A perf + quantization brief** (not yet written) — absorbs `04f` vertex quantization + probe-GI cost reduction + general perf-regression hunt. Runs after 08, before Phase B (hard deadline: quantized/skinned vertex format must be locked before Phase B skinning).
17. `10-style-checkpoint.md` — Asset style validation (process, not feature)

**Tooling/UI arc dependency order** (briefs 11–15, inserted 2026-07-25): dependency-wise `11` is
independent of the UI track (`12` → `13`), and `14` needs `11`+`13`, `15` needs `11`+`13` plus
`14`'s picking/gizmo subsystem. **Execution is SERIAL, `11` first** (not parallel, despite the
lack of a hard dep) — concurrent agents share the `.#demo` build and break each other's baselines
(04c/05 lesson). Scope line for 14/15: render/pipeline debugging + bounded lookdev/light/material
manipulation ONLY — the Tier-5 scene/entity authoring editor stays Phase B.

## Conventions all agents must follow

- **Build/verify**: `nix flake check` must pass; demo builds via `.#demo`; visual
  verification is `./run.sh` — but agents CANNOT see the screen. Any milestone
  whose acceptance is visual must end with the agent reporting "NEEDS VISUAL
  VERIFY" plus exactly what the user should look for (and what failure looks
  like). Never claim visual success.
- **New files need `git add -N`** or the nix git-source build won't see them.
- **Layout**: engine code in `string-engine/` (`string::`/`string::gpu`
  namespaces), demo/game-side code in `sandbox/`. Engine stays generic — asset
  formats, game policy, and scene specifics live sandbox-side (precedent: KTX in
  the loader, `TransferBatch` format-agnostic).
- **Tests**: engine-level logic gets gtests in `string-engine/test/` (precedent:
  `residency_manager_test.cpp`).
- Match existing code style; comments only for non-obvious constraints.
- Update the relevant brief's "Status" line when a milestone lands.

## Verification rules (learned the hard way)

- **Perceptual/culling gates MUST run at MULTIPLE resolutions, including a
  non-square one (e.g. 800x800, 1600x1600, 1920x1080) — NOT 800x800 only.** An
  800x800-only blind spot helped ship a HiZ over-cull + no-depth-test phase-2 bug
  (see 04d's 2026-07-23 fixes): the pyramid sparse-mip error scales with mip count
  and was invisible at 800. Use `STRING_WINDOW_SIZE=WxH`.
- **Judge captures at a view with LAYERED geometry** (a near surface with distinct
  geometry behind it — e.g. a ceiling with a roofline beyond, NOT a wall backed by
  sky). Far-over-near compositing errors at a sky-backed view read as "shading" and
  were signed off clean while the user saw obviously see-through surfaces.
- **When a capture disagrees with a live report, TRUST THE LIVE REPORT.** Levers:
  `STRING_FIXED_DT=<sec>` + `STRING_DBG_ORBIT` align motion A/B captures by frame
  index; `STRING_SERIALIZE_FRAMES=1` (waitidle per frame) discriminates races from
  logic bugs — if an artifact survives it, it is NOT a frames-in-flight race. Note
  `capture_color_target` itself drains the GPU, so captures can never show such races.
- **A pass recorded into ANOTHER group's render-pass instance inherits that group's
  attachments.** The 04d transparency bug lived in the renderer's group-forming
  `depth_target_of` (it ignored DepthRead, silently unbinding depth and stripping
  phase-2's depth test) — not in the pass or shaders where all auditing focused. If
  draws piggyback on a host group, the host's attachment derivation is part of the
  feature's correctness surface.

## Phase A exit gate (tracked across briefs, not one agent's job)

Validation-layer clean; resize/alt-tab solid; Windows build in CI; frame budget
holds in the synthetic stress scenes (light stress from 02, crowd stress from 03,
overdraw stress from 08); sandbox scenes pass the style checkpoint; UI mock
screens feel satisfying (user judgment).
