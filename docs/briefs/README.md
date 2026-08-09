# Phase A implementation briefs

Each file is a self-contained brief for an Opus implementation agent, in execution
order. Decisions in these briefs were made with the user (2026-07-21 walkthrough);
agents implement them, they do not re-decide them. Parent spec:
`docs/engine-feature-spec.md`.

## Order

**IMMEDIATE — `21-vision-alignment.md`** (2026-08-07). Runs NEXT, before everything below. Brief 20
(the render-graph declaration unification) LANDED and is committed as `12c1ebc`; the post-landing
audit found the drift did not disappear so much as move around the graph, and that several of the
brief's own completion claims were false. Brief 21 is the alignment pass that closes that out —
doc-record repair first, then seven code steps. Nothing else is built until it lands: briefs 12-15
rest on brief 11 deliverables that this arc is still reshaping.

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
12. `07-pbr-ibl.md` — Dynamic sky IBL, physical light units, EV exposure — **DONE** (user-verified 2026-07-23)
13. `09-post-processing.md` — Bloom, auto-exposure, GTAO, ACES 2.0 LUT — **DONE** (pulled ahead of 08; defect marathon user-verified 2026-07-24; SMAA deferred)
14. `09b-probe-gi.md` — Relightable irradiance probe volume — **FUNCTIONALLY CLOSED** (2026-07-25; GI + CSM done, runtime cost deferred to the perf+quant brief; HW-RT backend = Phase 2)
15. `20-graph-declaration-unification.md` — One declaration channel over logical handles; app owns the graph; pass base class deleted — **LANDED + COMMITTED `12c1ebc` 2026-08-07** (read its STATE BANNER first; sections are non-chronological)
16. `21-vision-alignment.md` — Post-audit alignment: four user adjudications (framework-opens ratified, soft authored-once, time-outside-the-graph, FrameScratch deleted), doc repairs, then code steps 1-7 — **IN PROGRESS 2026-08-07**
17. `11-pipeline-modularity.md` — Decompose the GeometryPass monolith into registered passes; graph abstraction update; introspectable+mutable light/material state — **WRITTEN + LARGELY SUPERSEDED**: the graph/pass half landed via briefs 20/21 (see their supersession notes inside 11); the introspection + light/material store half (M4) is still open and is the seam briefs 14/15 need
18. `12-ui-panels.md` — Full ImGui-like dockable/splittable/tabbed panel system + layout persistence — **COMPLETE 2026-08-01** (M0 facade + M1 floating panels + M2a-d workspace/docking, all user-verified; M2e persistence and M4 retained seam deferred with reasons in the brief). *This index said "written; not started" until 2026-08-08 — it was wrong; the code is in `string-ui/src/{panel,workspace}.cpp`.*
19. `12b-ui-frame-architecture.md` — UI frame architecture: shaping cache, frame arena, surfaces in layers, placement, clean-surface skip. **COMPLETE 2026-08-03** — M0, M0b, M1, M2, M3 all landed and gated.
20. `12c-retained-regions.md` — Retained regions: skip AUTHORING for subtrees whose tracked handle reads are unchanged, then the whole UI pass when nothing is dirty. This is brief 12's deferred M4, scoped once 12b's measurements showed authoring is ~96% of the UI frame at stress scale (and is paid every frame — it was never amortized). Dirt model chosen with the user: auto-tracked handle reads compared by value, with `STRING_UI_VERIFY_RETAINED=1` as the net. **CLOSED as premature** (the measurement that motivated it was an `-O0` artefact).
21. `22-optional-ui-libraries.md` — **Runs BEFORE 14/15 and changes their substrate.** The debug tools are re-authored on Dear ImGui behind a bridge library (`ImDrawData` on our bindless path so `ImTextureID` is a slot, one declared leaf pass, `ImGuiIO` driven from `string::Input` through the brief-17 context stack); `string-debug` ports onto it and drops its `string-ui` dependency, making "optional per distribution" real. Port set: console, logs, F1 tab bar (now the dockspace host), lens — then a second wave, then new tooling (file viewer, read-only node graph over the render graph). Deps LOCKED 2026-08-08: ImGui **docking branch** + **`imgui-node-editor`**, with **multi-viewport OFF** (it needs the platform backend and N swapchains). **`string-ui` is NOT touched** — brief 12's docking, 12b's frame architecture and the 5 UI dump baselines all stand. *(DRAFT 2026-08-08, not started.)*
22. `13-ui-widgets.md` — Widget set (sliders, drag-values, collapsibles, tabs, tables/trees, color) inside the panels; immediate-mode CPU cost *(written; not started — **narrowed, not superseded, by brief 22**: still needed for the production kit, no longer on the critical path for debug tooling.)*
23. `14-debug-ui-inspect.md` — Debug tools in the UI: render-target visualizer, material/G-buffer channel isolation + material-preview sphere, per-pass toggles + inline timings, live CVar sliders. *(Status line says DRAFT, but `author_dag` / `author_image` / `author_lenses` already exist in `string-debug/src/panels.cpp` — the brief was never updated. Re-inventory against the code, not the brief; brief 22 M0 does this, and brief 22 is where this content gets re-authored.)*
24. `15-debug-ui-manipulate.md` — Interactive debug: transform gizmos + live material-property editing *(NOT yet written — the only one of 11-15 that isn't. Brief 22 changes its substrate: gizmos would ride ImGuizmo rather than the bespoke widget kit.)*
25. `17-input-routing.md` — Unified input: one binding table + a context stack arbitrating between UI and gameplay, interned action ids. **M1+M2 done 2026-08-03** (contexts + interned ids; UI routed actions + focus scopes). **M3 done** (context priority tiers, system actions, F8 binding inspector/rebinder). Slots beside the UI arc because the UI's hardcoded key signals are what M2 migrates.
26. `18-asset-scene-import.md` — Asset import + data-driven scenes: `.scene` descriptors discovered from disk, cook-on-import (geometry + BC7 textures), rescan/import in the Scene menu, plus **M4 Shadertoy scenes** (a descriptor naming a shader instead of models — fullscreen pass + standard inputs, riding the brief-01 hot-reload keep-last-good loop; no in-engine code editing). Closes the gap left by runtime scene switching — scenes are still C++ lambdas with hardcoded asset paths, so an artist's glTF needs a rebuild. Scope is *view this asset / prototype this shader*, NOT scene composition. **Extended 2026-08-08 with M5 — a second source format** (ufbx or assimp): the engine already cannot see an importer (`string-asset-tools` is tools-only) and `bake_scene()` is already the documented extension point, so M5a is a pure seam refactor gated on a BYTE-IDENTICAL cook, and M5b is one front-end `.cpp` plus its own conformance tests. *(DRAFT 2026-08-04, not started.)*
27. `08-vfx-telegraphs.md` — GPU particles, telegraphs, decals
28. **End-of-Phase-A perf + quantization brief** (not yet written) — absorbs `04f` vertex quantization + probe-GI cost reduction + general perf-regression hunt. Runs after 08, before Phase B (hard deadline: quantized/skinned vertex format must be locked before Phase B skinning).
29. `10-style-checkpoint.md` — Asset style validation (process, not feature)

**Reshaped by brief 22 (2026-08-08):** the arc below was written assuming one UI kit serves both debug
and product. Brief 22 splits the CONSUMERS — ImGui for debug tooling, `string-ui` unchanged for
product — so `22` runs before `14`/`15` and changes what they are built on. `13` is narrowed to the
production kit rather than superseded. Read `22` first.

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
