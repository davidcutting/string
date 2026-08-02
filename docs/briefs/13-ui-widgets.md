# Brief 13 — UI widget set

Status: **DRAFT / not started** (planning). Inherits the UI API conventions locked in
`12-ui-panels.md` (names / callbacks / handle-binding / closures / Clay-sizing / central theme /
retained-mode seam) — do not re-decide them here. Part of the tooling/UI arc (11–15) before 08 VFX.
Deps: builds on brief 12 (panels); consumed by briefs 14–15.

## Goal

The widget set the debug UI (14/15) and matured game UI need, built inside brief 12's panels and
authored through brief 12's conventions. Also: bring down the **immediate-mode CPU cost**
(~4ms measured in brief 05).

## Scope — widget set

All follow the locked conventions (explicit name, `.label()`, callbacks + poll sugar, `bind(...)`,
chainable sizing/theme overrides):

- **Sliders** + **drag-values** (numeric scrub), int/float, `.range()`, `.format()`, `.step()`.
- **Checkbox / toggle**, **combo / dropdown**, **radio**.
- **Text field** (reuse brief-05 input path; handle-bound string via the store).
- **Collapsible / section header**, **tab content** (pairs with brief 12 tab bars).
- **Color picker** (RGB/HSV, swatch).
- **Tables / trees / graphs** — see Dynamic collections below.
- **Image** widget — samples a render-target/texture handle with channel / mip / range /
  false-color controls. This is the primitive brief 14's render-target visualizer + material
  preview sphere are built from, so it lands here.
- Existing brief-05 widgets (panel / button / progress / tooltip / icon_cell + cooldown mask) fold
  into the set through the new conventions.

## Dynamic collections (tables / trees / graphs) — LOCKED 2026-07-25

**Adapter-backed with loop-like ergonomics**: the widget takes a count + per-row/node render
callback + a `.key`; the framework controls *how often* it calls you (virtualization). Plain
`.content` + a `for` loop stays available for small static sets — no forced adapter ceremony.
The adapter (count + renderer + key) *is* the retained-friendly model, so this choice serves both
the CPU-cost and retained-mode goals at once.

- **Tables**: `.columns(...)` + `.rows(count, render, key)`. **Full virtualization** (only visible
  rows built — the direct CPU-cost lever). **Sorting + filtering v1**. **Resizable + reorderable
  columns v1**. Single-row **select + hover v1**; **multi-select later**. Per-row identity via `.key`.
- **Trees**: `.nodes(roots, render, get_children, key)`. Expand-state persisted by key; virtualized
  (only visible/expanded nodes built).
- **Graphs (node/edge)** — a structural type beyond trees, for visualizing DAGs/graphs. Primary
  consumer: the **render-graph pass DAG** (brief 14 wires passes→nodes, resource deps→edges).
  Adapter-consistent: `.nodes(count, render, key)` + `.edges(deps, from_key, to_key[, ports])`;
  node bodies can host real widgets (a pass node can show its toggle + timing inline). **Pan/zoom
  canvas**, hover/select with **edge highlight**. **Layered/topological auto-layout v1** (ideal for
  the render-graph DAG) with **manual drag-reposition persisted by key**; force-directed layout for
  general/cyclic graphs = optional-later. **Viewport-cull off-canvas nodes** v1; true large-graph
  virtualization later. Heaviest widget (canvas + layout algo + edge routing) → its own milestone;
  **may split to its own brief if it balloons**. Inherently retained-ish (positions/pan/zoom persist
  by key).

## Immediate-mode CPU cost

Brief 05 measured ~4ms average for the immediate-mode rebuild. Bring it down (candidates:
per-element work reduction, memoization of unchanged subtrees, cheaper text shaping/measure reuse,
cutting redundant layout passes). Target + method set when the brief is finalized.

## Milestones (first cut — refine before spawning)

- **M0** — Core input widgets (slider, drag-value, checkbox, combo, text field) through the facade.
- **M1** — Structural widgets (collapsible, tab content, color picker).
- **M2 — DEFERRED to brief 14 (user, 2026-08-01), not cancelled.** Image widget
  (channel/mip/range/false-color).

  **Still wanted, for a reason the lens below cannot cover: the lens only works for SCREEN-SPACE
  targets.** Substituting in place is meaningful for normals, roughness, AO, overdraw — anything
  corresponding pixel-for-pixel with the frame. A **shadow map is in light space**; a cubemap face, an
  IBL atlas and a probe atlas have no screen position to substitute at. Those can ONLY be inspected in
  a panel. The two tools cover disjoint sets of targets, not overlapping conveniences.

  Deferred because its consumer is brief 14, and because the risky half is not the widget — it is
  SAMPLING A RENDER TARGET FROM THE UI PASS, which needs a dynamically declared `SampledRead` on
  whatever target is currently selected (expressible: brief 16 made `PassSpec.usages()` a live
  getter). Brief 14 has to solve that anyway, so building the widget here would land the easy half
  early and leave the hazards for later.

  What it needs when it lands, from an actual read of the code: the sampling path already exists —
  the text shader does `textures[pc.atlas_slot].Sample(...)` against a bindless `Sampler2D
  textures[]`, with slots from `descriptor_table_.get_binding_slot(image, TEXTURE)`. So the work is
  (a) `element.image` as a 1-based index into a builder-side table, mirroring how `text` is held out
  of line to keep `element` small; (b) a third stream in `ui_pass` (pack + pipeline + a draw inside
  each layer batch, which M1b's per-layer batching already accommodates); (c) a shader doing channel
  swizzle, `SampleLevel` for mips, range remap and false-colour; (d) the widget API.
  **De-risking note: the glyph atlas is already a bindless sampled texture, so the whole widget can
  be built and verified against it with zero barrier risk before any render target is involved.**
- **M3** — Tables + trees: full virtualization, sorting + filtering, resizable + reorderable
  columns, single-row select + hover, persisted tree expand-state.
- **M4** — Graph widget: node/edge canvas, layered/topological auto-layout, pan/zoom, manual
  reposition persisted by key, edge highlight, viewport cull. (Split to its own brief if it balloons.)
- **M4b — Scroll + wheel (DONE + USER-VERIFIED LIVE 2026-08-01).** The four pieces the user asked for as "a bow on the
  current functionality": mouse wheel, signed layout coordinates, scissor clipping, scroll area +
  graph zoom. Text wrap is the one still outstanding.

  Three primitives landed, each load-bearing for the next:
  - `element.wheel` — a container declares that it consumes the wheel; `hit_test_layered` walks from
    the hit node to the nearest marked ancestor and publishes `interaction::wheel`. **Arbitration
    lives in the hit test because only the tree knows the ancestry**: the cursor is over a ROW, and
    the thing that must move is the list. Innermost wins, so a graph inside a scroll area behaves.
    Widgets read one call, `wheel_for(id)`, and never ask whether they are hovered.
  - `Ui::observed_box(id)` / `Ui::observe()` — the OPT-IN, per-id post-layout box cache that
    `interaction::hovered_box` deliberately is not. A scroll area needs its content HEIGHT, which is
    precisely what the layout computes and the author cannot state. One frame stale, which is fine
    for an extent and would not be for a position. Wired at the same host seam as
    `Workspace::observe`.
  - `ScrollArea` — the general case the table and tree solve narrowly. They are virtualized (uniform
    rows, so build only the visible ones); arbitrary content cannot be, so it is all built and
    CLIPPED. That is why it needed the scissor and they did not.

  Graph zoom is exponential in notches (equal proportional steps), clamped to 0.35..3.0, anchored on
  the canvas centre — cursor-anchored zoom would need the canvas's screen rect, which the widget
  deliberately does not know. Node drag offsets are stored in graph units and divided by the zoom.
  Table and tree take the wheel in whole ROWS, since their scroll position IS a row index.

  **Defect found while gating, worth keeping:** the wheel target was first resolved only on frames
  with notches. That reads as an optimisation and is an off-by-one-frame — notches arrive in
  `begin_interaction` at the top of a frame, the tree they resolve against is laid out at the end of
  the previous one, so the wheel needed two consecutive notched frames to move anything. It is now
  resolved every frame, exactly like `hovered`. The unit test that caught it splits the two host
  calls rather than faking one combined step.
- **M4c — Text wrap (DONE 2026-08-01).** The last of the five. Opt-in via `.wrap()`, so no existing
  screen re-flows; verified by the 5 dump baselines staying at 0 differing.

  **We read Clay's implementation before designing this** (`clay.h`, `Clay__CalculateFinalLayout`)
  and took its structure, because the apparent circularity — height needs width, width needs height —
  is not real: **width never depends on height.** So the passes are ordered rather than iterated:

  1. bottom-up fit (unchanged),
  2. flex **width**,
  3. **wrap seam** — ask each opted-in element for its height at the width just resolved, then
     re-fit heights bottom-up so the new height reaches its ancestors,
  4. flex **height**,
  5. position (unchanged).

  Steps 2 and 4 are the same function with an axis parameter. A container touches X in exactly one of
  two ways — it DISTRIBUTES along X if it is horizontal, STRETCHES along X if it is vertical, never
  both — so the split is a gate on which block runs, not a reorganisation, and the top-down order
  within each axis is unchanged. That is why the split alone moved no baselines.

  **The measurer is three operations now** (`measurer` concept in layout.hpp): the unwrapped size,
  `min_width` (the widest WORD — the shrink floor, so a wrapping box is an ordinary shrinkable box to
  the width pass), and `height_at(e, width)` (the only one that runs mid-layout). Clay's equivalent
  of the third is its per-word measurement cache; **we do not need one**, because our wrap is a single
  linear pass rather than a search over candidate break points. If wrapping ever gets expensive, that
  cache is the known answer.

  **Where we deliberately differ from Clay:** it wraps all text by default and stores wrapped lines
  for the renderer to draw one command each. Ours is opt-in, and the renderer re-derives the wrap from
  the final box width — which is safe only because `dynamic_text_measurer::shape()` and the ui_pass
  shaper run the *identical* algorithm with identical whole-pixel advance rounding. That contract was
  already load-bearing for fixed-width text; `.wrap()` only widened the gate. **If those two ever
  drift, text draws a different line count than its box reserves.** Clay's store-the-lines approach is
  the more robust one if that ever bites.

  Not carried over: Clay trims the trailing space from a wrapped line's width (`finalCharIsSpace`).
  Ours counts it, which can make a measured line marginally wider than it draws. Cosmetic, and it does
  not affect line count.
- **M5** — Immediate-mode CPU cost reduction to target.

## Verification (per `README.md` rules)

- Every widget: hover/focus/drag/keyboard + gamepad focus nav correct; handle binding round-trips
  (edit → store → re-read) with no dangling; theme inherit + per-element override both apply.
- Determinism: same inputs → same layout/shaping (whole-pixel advances, per brief-05 rules).
- Immediate-mode CPU cost hits target in the 500-nameplate + debug-panel-heavy scenes (Tracy).
- **NEEDS VISUAL VERIFY** (widget feel, color picker, tables/trees, image widget correctness).
