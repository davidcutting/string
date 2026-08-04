# Brief 12b — UI surfaces + layers: the shaping cache, placement, and clean-surface skip

Status: **DRAFT / for discussion** (written 2026-08-03, from a design review with the user of an
external retained-mode arena proposal against the shipped string-ui core; vocabulary reworked with
the user 2026-08-03). Inherits the UI API conventions locked in `12-ui-panels.md` (names /
callbacks / handle-binding / closures / Clay-sizing / central theme / retained-mode seam) —
nothing here re-decides them. **Absorbs brief 13's "Immediate-mode CPU cost" line item** (13
deferred target + method to "when the brief is finalized"; this is that brief). Deps: brief 12
panels + brief 13 widgets as they exist today. Consumers: brief 14 (inspector anchoring wants
M1), the deferred brief-12 M4 retained seam (M0–M3 are its load-bearing prerequisites), the
brief-13 graph canvas (M2 seeds its transform).

## Goal

Restructure the UI frame around **surfaces in layers** instead of one megatree, stop re-deriving
**text shaping** and **layout for clean surfaces** every frame, and classify element fields into
**input classes** (layout inputs / placement / paint) so the skip logic is correct under
animation. Every milestone must pay for itself in the current immediate regime AND be a
load-bearing piece of the future retained-on-immediate layer — the same rule as the render-graph
arc: no scaffolding, no wrappers, each step real on its own.

## Vocabulary

The mental model is a small windowing system — the shape that has carried these exact concepts
for forty years — with one deliberate departure: **"window" is reserved for the OS window (WSI)**
and never names a UI-side concept. Wayland makes the same reservation for the same reason: the
content primitive is a *surface*; a window is the OS-level thing a surface is hosted in.
`Workspace` tiles docked surfaces; `PanelStore` manages floating ones — the window-manager roles,
applied to surfaces; neither is renamed, the vocabulary gives them roles.

- **Surface** — an element subtree authored and laid out as one unit: a panel, the docked
  workspace, a nameplate, a popup, the console. In the builder it is a contiguous range of the
  node pool (each outermost tree's pre-order nodes are already contiguous — this falls out of
  sequential authoring; no new storage machinery). The word is already in the codebase with this
  meaning ("a modal surface (debug console, HUD)", layout.hpp) — this brief promotes it from
  informal to structural. NOT reused for `Theme::surface` (a color ROLE — a surface is painted
  with the surface color; rhyme, not collision). On the WSI homonym (`VkSurfaceKHR`,
  `wl_surface`): accepted, 2026-08-03. The platform layer already juggles three surface types
  per file, disambiguated by their qualified names — and the build guarantees the zones never
  mix (string-ui depends on nothing; `ui_pass` touches no WSI). Rule: bare "surface" belongs to
  `string::ui`; WSI-side is always qualified ("window surface" — Vulkan's own phrase —
  `VkSurfaceKHR`, `wl_surface`), which is what that code already does. Every alternative word is
  taken by a locked concept (view, region, canvas, card, screen, pane), so a rename would buy a
  worse word to dodge a collision that occurs in zero files.
- **Panel vs surface** (raised and settled 2026-08-03) — a panel is FURNITURE (title bar, tabs,
  grip — content + behavior, brief 12's vocabulary); a surface is a FRAME-SCHEDULE unit
  (placement + signature + layer). Neither subsumes the other: a FLOATING panel is hosted 1:1 by
  its own surface; a DOCKED panel (`workspace_node::kind::Panel`) is ordinary content inside the
  workspace's one surface; nameplates, popups and the console are surfaces with no panel at all.
  **Docking is re-hosting**: dock = the panel's content joins the workspace surface and its own
  surface disappears; tear-off = it gets one again. Identity (name hash, cards, remembered rect)
  lives on the PANEL and survives every hop; hosting lives on the SURFACE and changes at each
  hop — which is why the words must not merge (collapse them and "dock this panel" reads as
  "destroy this panel"). The ladder extends one rung to the tear-off future: content in a shared
  surface -> own surface -> own window.
- **Placement** — where a surface sits: rect + layer + z. Position and size are DIFFERENT input
  classes: position (x, y, layer, z) never dirties layout; SIZE is the surface's layout
  constraint (the `available` its layout resolves against) and folds into the signature.
  **Resizing reflows; moving never does.** DATA, written by whoever owns the
  surface's position — the workspace for docked surfaces, `PanelStore` for floating panels, the
  world projection for nameplates, anchor logic for popups. Deliberately NOT a reified
  "provider" object: the house pattern is plain writes by an owner (`update_panel` writes
  `panel_state.rect`; it is not a Provider and never was). Rhymes with `workspace::placement`
  ("where a card goes") one level up; the C++ types stay distinct.
- **Layer** — ordered stacking bucket of surfaces. Layer N is authored and laid out only after
  layer N-1 is RESOLVED, so **a surface may only anchor to lower layers** — same-frame anchoring
  is acyclic by construction. Generalizes the `element.overlay` boolean (documented as "topmost
  render LAYER") into an ordered list; element-level `z` keeps its meaning WITHIN a surface, and
  surface-level z orders surfaces within a layer, exactly as before.
- **Surface coordinates** — boxes inside a surface resolve local to the surface; the placement
  rect is where the surface sits in the host window's client area (today's `screen` space).
  Published boxes (`hovered_box`, `observed_box`) stay SCREEN-space so widget math is untouched.
- **Input classes** — which product an element field feeds: **layout inputs** (sizing, format,
  structure, text content, wrap), **placement** (the surface rect — never per-element),
  **paint** (colors, stroke, radius, sweep, z). Motion writes paint by default. Not called
  "tiers" — Tier-N is the roadmap's word.
- **Signature** — FNV-1a over a surface's layout inputs, folded in during authoring. Unchanged
  signature => the surface is **clean** and last frame's resolved boxes are still valid. Same
  word, same discipline as the renderer's enabled-set signature (compile once, recompile on
  signature change) — the rhyme is deliberate.

Metaphor boundaries, so nobody extends it wrong: nothing here is named "compositor" (the post
stack owns "composite", and this system orders/offsets/routes — it does not blend); the M3
skip is NOT "damage tracking" (compositor damage is pixel regions to repaint; this is layout
invalidation at surface granularity); and "window" stays OS-only because the docking system may
plausibly grow tear-off-to-native someday (drag a surface against the window edge and it spawns
a NEW OS window hosting that surface — ImGui-viewports-style). That sentence is coherent exactly
because surface and window are different words; placement would grow a window id then, and
nothing else in this vocabulary moves.

## Motivation — measured, not assumed (2026-08-03)

Benchmark: real `Ui` facade + widgets + panels + the real SDF `dynamic_text_measurer`, 2560x1440,
500 frames averaged, clang -O2 (harness: `ui_bench.cpp`, to be promoted into `string-ui/test/` as
M0's gate). Author = facade authoring (hashing, arena copies, Motion, widget emission); layout =
`end(screen, measurer)`; resolve = both interaction resolvers.

| scenario                                    | nodes | author | layout  | total/frame |
|---------------------------------------------|-------|--------|---------|-------------|
| debug shell (3 panels x 12 rows)            |  202  |   7us  |   41us  |  **49us**   |
| heavy tooling (10 panels x 30 rows)         | 1311  |  42us  |  278us  |  **322us**  |
| shell + 200 nameplates                      |  402  |  13us  |  315us  |  **330us**  |
| HUD stress (1000 nameplates)                | 1085  |  33us  | 1419us  |  **1.46ms** |
| extreme (10x60 rows + 2000 plates)          | 4211  | 127us  | 3318us  |  **3.46ms** |

Re-running the identical trees with `no_measure` (structural layout only): "extreme" layout drops
3318us -> **156us**; HUD stress 1419us -> **35us**. So:

- **~95% of layout cost is text shaping**, not the fit/flex/wrap/position passes. `shape_text`
  re-decodes UTF-8, re-kerns and re-walks glyph metrics every frame for strings that are
  overwhelmingly identical to last frame's.
- **Every string is shaped TWICE per frame**: once in `dynamic_text_measurer` to size it, once in
  `ui_pass.cpp` (`shape_text` with a glyph callback) to place its quads. The real per-frame text
  cost is roughly double the table's layout column.
- The structural passes are cheap (156us at 4.2k nodes) — the flat pool + pre-order design is
  fine and is NOT what this brief changes.
- Movement is structurally conflated with reflow: `float_x/y` are layout inputs, so a nameplate
  that moves 1px re-lays-out AND re-shapes its subtree. The defect is obvious by analogy:
  **dragging a surface should never re-lay-out its contents**, the way dragging an OS window
  never re-lays-out the app inside it — today it does, because position and content are the
  same input class.

This matches brief 05's old ~4ms measurement and locates it: the immediate-mode CPU cost is text
re-derivation plus the absence of any unit of skipping. The remedy is NOT the wholesale retained
arena (see "What we are deliberately NOT taking"), it is four bounded moves plus one micro
Motion field (M0b).

## Relationship to the retained-mode proposal (context for reviewers)

An external design document (retained arena, SoA columns, CSR root set, dirty bitsets, animator
tiers, providers/phases) was reviewed against this codebase 2026-08-03. Verdict agreed with the
user: as written it is a retained CORE (writers mutate a persistent tree; no reconciler, no
per-frame authoring), which conflicts with locked decisions — but most of its components are
agnostic about who the writer is, and it becomes the retained-on-immediate design brief 12
reserved the moment a reconciler is added. Its genuinely transferable ideas are exactly the four
below: shaping-as-cached-product, surfaces with owner-written placement in ordered layers,
position-outside-layout, and input classification. The last one is not an optimization but a
CORRECTNESS requirement for any skip logic: without an input-class mask, every Motion hover fade
dirties its panel and skipping degenerates to today's full rebuild. That insight is imported; the
retained core is not.

Naming diverges from the proposal deliberately (2026-08-03): its "Root" collides three ways in
this codebase (`tree_node::is_root`, `Workspace::root_`, "the host owns the root"), its "Phase"
collides with two-phase occlusion / `geometry.phase1/2` / Phase A-B, its "Tier" collides with the
roadmap's Tier-N, and its "Provider" reifies what the house pattern treats as plain writes. The
proposal itself listed *Surface* as the informal term for root+subtree; this brief promotes it.

## The model

### Frame shape (after M1–M3)

```
begin_interaction                       // press/drag from last frame's tree (unchanged)
for layer in layers:                    // ordered, bottom to top
    for surface in layer (by z):
        owner writes placement          // panel drag / world anchor / popup anchor land HERE,
                                        //   reading resolved boxes from LOWER layers only
        author(surface)                 // immediate closures, unchanged API; folds signature
        if signature changed:           // layout inputs only
            layout(surface)             // fit/flex/wrap/position, SURFACE coordinates
            measure via shaping cache
        else:
            reuse last frame's boxes    // clean surface: copy box column for the range
resolve_interaction                     // hover/focus vs the new tree (unchanged semantics)
paint: layers bottom-up, surfaces by z, placement offset applied at draw
hit-test: the same order walked top-down
```

Invariants (pin to the wall):
1. Authors and the two resolvers are the only writers; layout/paint only read.
2. A surface may only anchor to lower layers.
3. Placement POSITION changes (drag a panel, move a nameplate) re-run NO layout and NO shaping.
   Placement SIZE is a layout constraint: resizing reflows, moving never does.
4. Paint changes (Motion fades, sweep ticks) re-run NO layout and NO shaping.
5. Only layout inputs enter the signature, so only layout-input changes reach the layout
   passes — and only for their own surface.

### Animation under this model

The reviewed proposal had registered Animators bound to (element, column, tier) so each write
could ANNOUNCE what it dirties. 12b does not import them, and the reason is the same one the
render graph retired hand-rolled barriers: **dirt is derived, not declared**. Motion stays
pull-based and untouched (authoring always runs — the skip is layout-only — so its tick/aging
need no changes). The three animation classes then fall out of the input classes:

- **Paint** (hover fades, sweep): excluded from the signature; renders on CLEAN surfaces because
  the skip restores only the BOX columns — element paint fields are always this frame's authored
  values.
- **Placement position** (console slide-down, popup slide-in, a panel gliding to its dock): FREE.
  Animate the rect's position; the surface glides with zero relayout and zero reshaping. Under
  the megatree every slide re-positioned its subtree per frame; here it is one write.
- **Layout inputs** (a collapsible easing open): the author writes the animated value, the
  signature diffs, the surface comes out dirty — the proposal's layout-tier animator with no
  registry and no tier tag on any writer; the classification lives once, in the signature mask.
  Cost is honest: a full relayout of one dirty surface (~10us) per animated frame, which is what
  correctness requires.

FLIP-style transitions (an element gliding from its old box to its new one after a layout
change) are NOT morph groups and are not rejected with them: they are a later Motion feature —
compare an id's box to last frame's, ease a paint-time offset — and M3's per-surface retained
box storage provides the "before" boxes for free. Out of scope here; noted so the enabling is
deliberate.

Coverage check (2026-08-03) — the model was audited against the 12 UX-motion principles
(easing, offset/delay, parenting, transformation, value change, masking, overlay, cloning,
obscuration, parallax, dimensionality, dolly/zoom). Nine are done or pure authoring under this
brief (easing/curves exist; parenting = the tree + placement + the author closure; value change
= animate + format; overlay = layers; cloning = a temporary popup-layer surface flying between
retained boxes; parallax = per-layer placement offsets; rect masking = animated clip;
size/corner transformation = layout-input + paint animation). The gaps, none structural:
`Transition.delay` — IN SCOPE, M0b (stagger is author-side index math); shaped masking and
backdrop blur are self-contained UI-shader/renderer features (the `sweep` cooldown wedge is
already a shaped mask in that shader, so there is precedent); and EVERYTHING else — zoom,
dolly, card-flip dimensionality — funnels into the ONE reserved M2 extension (placement rect ->
per-surface affine, hit-test inverting it at the existing cursor-conversion seam). No principle
requires rethinking the model; that convergence is part of why the shape is believed right.

## Milestones

### M0 — Shaping cache — **DONE** (2026-08-03)

Cache `shape_text` results — line metrics AND origin-relative glyph placements — keyed by
(content hash, quantized scale, wrap width), with Motion-style frame-aging eviction. Lives beside
`dynamic_font_atlas` (which already caches rasterized glyphs; this is the same idea one level up:
raster cache -> shaping cache). Consumed by BOTH `dynamic_text_measurer` (metrics) and `ui_pass`
(replay placements offset by node position) so the double-shape becomes two lookups.

Notes from the code read:
- The measurer currently shapes with `no_glyphs{}`; the pass shapes with a placement callback.
  One cached entry serves both — the comment in `text_measurer.hpp` ("measurement and drawing are
  one algorithm asked two questions") becomes literally one CACHED answer.
- Atlas growth does NOT invalidate: glyphs bake once at `bake_px` and their metrics are stable;
  new codepoints only ADD entries. A full atlas re-bake (not currently a thing) would need an
  epoch folded into the key — note it, don't build it.
- Eviction: entries untouched for N frames dropped (same rule as `Motion::end_frame`), so a
  scrolled-away table doesn't pin its rows forever.

Expected from the benchmark: extreme 3.5ms -> ~0.6ms, HUD stress -> ~0.1ms, PLUS the unmeasured
draw-side halving. Gate: byte-identical UI captures (this is a pure optimization), the wrap-sweep
regression test still green, benchmark delta recorded in this brief, cache gtests
(hit/miss/eviction/wrap-width keying).

### M0b — `Transition.delay` (micro; standalone like M0)

Add `delay` (seconds, default 0) to `Transition`: the transition's clock starts when the target
is written; the value HOLDS at its current until `delay` elapses, then eases over `duration`
with `curve`. First-sight snap is unchanged (an entry's first frame still snaps to target — a
delayed ENTRANCE seeds its start by authoring the initial state, exactly as today). Stagger
needs no API: it is author-side index math (`{.delay = i * 0.03f}`), which the pull-based model
makes natural — that is why this is a field, not a choreography system.

Gate: Motion gtests (hold-then-ease timing; two staggered entries complete offset by their
delays; delay=0 is bit-identical to today), plus the existing aging behavior unchanged.

### M1 — Surfaces in layers

The frame becomes an ordered surface registry instead of one megatree. Suggested layers
(registry data, not hardcoded): [0] world (nameplates), [1] docked (workspace + screens),
[2] floating (one surface per floating panel; z from `PanelStore` order), [3] popups (combo
dropdowns, tooltips,
brief-14 inspector callouts — placed against resolved boxes from layers <=2 SAME-FRAME),
[4] modal (console/HUD). This retires the one-frame anchoring staleness by construction;
`Ui::observed_box` remains for extents (scroll content height), which is what it was for.

Mechanics, from the code read:
- `layout_builder` already lays out each outermost `begin...end()` independently and accumulates
  them in one pool, so multi-surface needs no builder rewrite — surfaces are recorded ranges.
- Paint iterates layers bottom-up (surfaces by z); hit-test walks the same order top-down. This
  SUBSUMES `hit_test_layered`'s overlay special-case and the `element.overlay` flag for whole
  surfaces: modal is a LAYER, not a per-element bit. (Element-level `z` inside a surface stays,
  for the shapes-then-text batching reason documented in layout.hpp.)
- The host seam (`UIPass::Author` -> one closure) becomes a surface registry — (placement
  owner, author) per surface, walked in (layer, z) order. Surfaces are ALSO DECLARED INLINE
  during another surface's authoring (`u.panel(...)` in a body; a combo's popup), and the
  declaration/emission split this needs is not ceremony but forced by layer order: a popup's
  declaring author runs while a lower layer is being built, and its layout cannot run until its
  anchor's layer has resolved. The codebase already made this exact move for the same reason —
  the Card model ("emission order IS tree structure, so the workspace, not the author, decides
  structure"): declaration stores the body closure, the schedule emits it at its layer. 12b
  generalizes the Card move to every inline-declared surface; `Panel`'s authoring API is
  unchanged. The existing single-closure path maps to one docked-layer surface, so the
  migration can be surface-at-a-time.

Two pieces the mechanics above imply but must be stated:
- **The anchor query gets a name**: `Ui::resolved_box(id)` — this frame's laid-out box for an
  element on an ALREADY-RESOLVED layer, screen-space. Legal only from placement owners / authors
  of HIGHER layers (asserted — an in-layer or upward read is the cycle the layer order exists to
  prevent). `observed_box` (one frame stale, any element) remains for extents; `resolved_box` is
  for anchoring. The post-layout observation seams (`Workspace::observe`, `Ui::observe`,
  `PostLayout`) are unchanged: they run once after ALL layers resolve, exactly as today.
- **Focus and directional nav become layer-aware**: `nearest_focusable` scores SCREEN-space
  boxes (conversion lands with M2) and never crosses layers upward past the topmost layer that
  contains focus — which is modal containment falling out of the structure (focus in the console
  cannot stick-nav down into a panel beneath it) rather than a special case. Wheel routing is
  untouched: it resolves by ancestry within the hit surface.

Gate: behavioral, not byte-identical — paint order and popup anchoring change ON PURPOSE where
they previously lagged a frame. Layout-dump tests per surface; capture compare at multiple
resolutions incl. non-square; NEEDS VISUAL VERIFY: drag a panel over the console (modal stays on
top), open a combo while moving its panel (popup tracks with zero lag — previously one frame).

### M2 — Surface coordinates; position belongs to placement

Boxes inside a surface resolve in surface coordinates; the placement rect is applied at paint and
at hit-test (cursor converted to surface coordinates per range; published boxes — `hovered_box`,
`observed_box` — stay SCREEN-space so widget math is untouched). Panel drags write placement
only. Nameplates become per-plate surfaces placed by the world projection — a moving nameplate
re-lays-out NOTHING (its signature is unchanged; its placement changed, which costs one write).

**Dragging a surface doesn't re-lay-out its contents** — that sentence is the whole milestone.

This is deliberately the degenerate (translate-only) case of a per-surface transform. The
brief-13 graph canvas's pan/zoom is the first consumer of the non-degenerate case — when zoom
lands, it extends this seam (a per-surface 2D affine) rather than adding a new concept. Not
built here; the seam just must not preclude it.

Mechanics note: everything derived from boxes offsets together — scissor rects (`clip`) and
glyph placements are computed from box + surface offset at draw, the same composition paint
already does. In-surface `floating` is unchanged and stays a layout input: scroll areas build on
it, and scrolling is DELIBERATELY layout, not placement — virtualization (tables/trees rebuild
from the first visible row) depends on scroll being content, so a scrolling surface is dirty
while it scrolls, and that is correct.

Gate: byte-identical captures for a static frame (offsets compose to the same pixels); live
verify for drag feel (panel tracks cursor exactly as before — the M1 "same frame drag" property
in `Panel` docs must survive); interaction gtests for hit-test coordinate conversion; nameplate
stress benchmark shows layout column ~= structural floor.

### M3 — Signatures: clean surfaces skip layout

While authoring appends a surface's nodes, fold its layout inputs into a running FNV: the
surface's layout CONSTRAINT (placement SIZE — resize must reflow; position deliberately NOT
folded), sizing, format, tree structure (parent/child counts fall out of append order), text
CONTENT (the string bytes + font_px, not the side-table index), wrap/clip/floating flags, and a
global epoch (theme + measurer identity — theme change dirties everything, correctly). At the
surface's `end()`:
signature match => the surface is clean => skip fit/flex/wrap/position AND all measurement,
restore the box column from last frame (per-surface retained box storage; structure is identical
by construction, so it is a column copy, not a diff). Mismatch => lay out as today.

Explicitly NOT in the signature: colors, stroke, radius, sweep, z — paint. This is the input
classification earning its keep: a Motion hover fade or a `sweep` cooldown tick must NOT dirty
its panel. Enforce with a gtest that animates every paint field on a panel for 100 frames and
asserts zero relayouts, and one that flips each layout input and asserts exactly one.

Interaction with M0: a dirty surface (a stats panel with live values) still hits the shaping
cache for its unchanged strings — the two skips compose; M3 without M0 would still re-shape
dirty surfaces' unchanged rows, M0 without M3 still walks structural layout for static panels.

Collision stance, stated rather than implied: the signature is 64-bit FNV-1a, and a collision
means a stale layout survives one edit. Accepted — the odds are ~2^-64 per surface-frame pair,
far below hardware error rates, and `STRING_UI_NO_SKIP=1` exists precisely to bisect any
suspected dirt bug. Not worth a 128-bit hash or a verify-mode compare.

Expected steady state at extreme scale: ~150–250us authoring (irreducible in the immediate
regime, and the price of the API staying immediate) + layout only for genuinely dirty surfaces.

Gate: byte-identical captures across an interaction script (hover sweeps, panel drags, live
values ticking) vs a build with skipping force-disabled (`STRING_UI_NO_SKIP=1` — keep the lever,
it is the A/B for every future dirt bug); relayout-count assertions as above; benchmark delta.

## What this buys the retained seam (why these moves, in this order)

Brief 12's deferred M4 ("retained-on-immediate") needs, in order: a persistent unit of identity
between frames (M1's surfaces; names within them — already locked), storage for resolved
products that outlives the frame (M3's per-surface box retention; M0's shaped runs), a
change-detection discipline that animation doesn't defeat (M3's input classes), and position
decoupled from content (M2). Full per-slot reconciliation then upgrades M3's surface-granularity
skip to element-granularity on the same substrate — an incremental step, not a rewrite. Retained
binds and callbacks-without-resite remain M4 scope, NOT this brief's.

## What we are deliberately NOT taking (decided 2026-08-03, do not drift into)

- **SoA/CSR/bitset element storage** — the measured bottleneck is shaping, not cache misses; and
  M0–M3 remove the workload it would optimize. Revisit only with a profile that says otherwise.
- **A retained element core / generational ElementIds** — names are identity (locked, brief 12);
  the tree stays authored per frame. Generations solve slot-reuse bugs a per-frame-rebuilt pool
  does not have.
- **Dock-slot placement computing split rects** — the workspace keeps expanding into layout
  (`workspace.hpp`'s defended invariant). The workspace's SURFACE gets a placement like
  everything else; its interior geometry stays the layout engine's.
- **Always-last-frame hit-testing** — the split resolvers (press vs last frame, hover vs this
  frame) are strictly better than the proposal's snapshot model; keep them.
- **Morph groups / cross-element SDF blending** — renderer feature, orthogonal, welcome later.
- **A general transform pass now** — M2 lands translate-only; the graph canvas is the first real
  consumer of scale and pays for the extension when it arrives.

## Verification (per `README.md` rules)

- M0/M3 are pure optimizations: byte-identical capture gates, multiple resolutions including
  non-square (`STRING_WINDOW_SIZE`), interaction scripts driven with `STRING_FIXED_DT` so A/B
  frames align.
- M1/M2 change behavior on purpose (layer unification, same-frame anchoring): layout-dump +
  capture gates re-baselined with the change described, plus NEEDS VISUAL VERIFY items listed
  per milestone above. Agents cannot see the screen; report what to look for and what failure
  looks like.
- The benchmark harness moves into `string-ui/test/` (headless, no device — the kit's
  dependency-freedom makes this free) and each milestone records its before/after in this brief.
  It is a REPORT, not a pass/fail gate — thresholds on shared CI hardware are flake factories;
  the numbers table above is the baseline.
- `STRING_UI_NO_SKIP=1` ships and stays: the permanent A/B lever for dirt bugs (M3), same spirit
  as `STRING_SERIALIZE_FRAMES` for sync bugs.

## Open questions (to settle while poking at this draft)

1. **Where does the surface registry live** — `Ui` (it already owns the frame) vs a new small
   object the host owns beside it? Leaning `Ui`, BUT a fact complicates it: the sandbox runs TWO
   `Ui` instances over one builder today (the fluent Ui + the debug shell's own, see
   `demo_scene.cpp` SharedUi/SharedPanels — both observed via the same PostLayout hook). Either
   the registry sits above both (UIPass-level), the shell's Ui folds into the app's, or each Ui
   registers surfaces into a shared schedule. Settle this FIRST in M1 — it decides the seam
   everything else hangs off.
2. **Per-plate surfaces vs one nameplate-batch surface.** Per-plate is the clean model (each
   plate has its own placement + signature) and hundreds of surfaces are fine by the numbers;
   but plate count is dynamic and surface identity should probably be `name-hash` like
   everything else. Confirm the registry handles high-churn surfaces (plates entering/leaving
   view) without signature-storage bloat — frame-aging again.
3. **Does M1 land before or after brief 14 starts?** 14's inspector anchoring is the first real
   popup-layer consumer; landing M1 first avoids 14 building on `observed_box` staleness and
   then migrating. Recommended: M0 immediately (it is independent), then M1 before 14.
4. **Signature granularity escape hatch.** A panel hosting one live-ticking row re-lays-out the
   whole panel every frame. Surface granularity accepts this (panels are small; layout of a
   dirty surface is ~10us). If a real case hurts, the answer is sub-surfaces (a card is a
   surface), NOT element-level diffing — that is M4's job.

---

## M0 — landed 2026-08-03

`text_shape_cache` (`string-ui/include/string/ui/text_shaper.hpp`, `src/text_shaper.cpp`) caches a
run's line metrics AND its origin-relative glyph placements, keyed by (content, scale, wrap). Both
consumers use it: `dynamic_text_measurer` takes the metrics, the renderer's `append_glyphs` replays
the placements. Aged per frame with Motion's rule (untouched entries are dropped). Owned by `UIPass`
beside the atlas; the measurer takes an optional pointer, so headless tests shape directly and the
cache is never a correctness dependency.

### The harness came first

`string-ui/test/ui_bench.cpp` — the numbers in the table above could not be reproduced from the tree,
so the design rested on measurements nobody else could regenerate. It now runs the real facade,
widgets, panels and SDF measurer headlessly, and **runs every scenario twice — cache off and on** —
so the milestone's result is re-derived on each run rather than recorded once.

It is a REPORT, not a gate: no thresholds, because a wall-clock assertion on shared hardware is a
flake factory. (It asserts only that the trees are non-empty, so a silently-broken scenario cannot
report a wonderful number.) Passing-test stdout is not in the build log; to read the table, run the
test binary directly or temporarily fail it.

### Measured on this machine (2560x1440, 200 frames, first 20 discarded)

| scenario                            | nodes | author | layout(off) | layout(on) | gain |
|-------------------------------------|-------|--------|-------------|------------|------|
| debug shell (3 panels x 12 rows)    |   124 |   63us |       290us |       43us | 6.7x |
| heavy tooling (10 panels x 30 rows) |   951 |  477us |      2474us |      366us | 6.8x |
| shell + 200 nameplates              |   524 |  272us |      1048us |      158us | 6.6x |
| HUD stress (1000 nameplates)        |  2124 | 1144us |      4299us |      593us | 7.2x |
| extreme (10x60 rows + 2000 plates)  |  5851 | 3061us |     13403us |     1843us | 7.3x |

Structural floor (`no_measure`): 28 / 201 / 89 / 337 / 1025us. So layout is now within ~1.8x of the
floor where it was ~13x — the remaining gap is cache lookups and the structural passes themselves.

**These numbers are NOT comparable to the table earlier in this brief**: that came from a different
harness that is not in the repo. This one is the baseline from here on.

**An honest surprise: authoring now dominates.** At the extreme case it is 3061us against 1843us of
layout. The earlier table showed authoring as small (42us at 1311 nodes), and the difference is that
these scenarios build names and values with `own("..." + std::to_string(i))` per row and per plate,
which is what the real nameplate and stats-row authors do. So M3's skip will not help the biggest
remaining cost, and M1/M2 do not either — the note in M3 that ~150-250us of authoring is
"irreducible in the immediate regime" is the right shape but an order of magnitude optimistic at this
scale. Worth confronting before M3 is scoped, rather than discovering it after.

### Decisions taken while implementing

- **Inputs are STORED and VERIFIED on every hit**, not just hashed. A collision here would draw the
  WRONG TEXT, which is louder than the stale layout a signature collision causes in M3 — and these
  strings average a handful of bytes, so verifying costs far less than the shaping it avoids. On a
  mismatch the entry is recomputed, so a collision degrades to a miss rather than a defect. (M3's
  stated 2^-64 stance is still right for M3; this one is cheap enough not to need it.)
- **Cached placements are UNCLIPPED.** The pass previously clipped glyphs to the node box *during*
  shaping. Caching that would poison the entry: the same string replayed at a different scroll offset
  would come back missing the glyphs that were outside the box the first time. Clipping is now purely
  a replay-time concern.
- Scale and wrap enter the key by BIT PATTERN rather than quantized — identical inputs give identical
  bits, so there is no tolerance to choose and no pair of nearly-equal scales silently sharing an entry.
- Aged AFTER the glyph packing, not after layout: both the measurer and the pass mark entries seen, so
  evicting earlier would drop runs the pass had not replayed yet.

### Verification

- **Byte-identical pixels, proven by A/B**: captured the UI scene with the cache, then with
  `append_glyphs` shaping inline instead of replaying, under `STRING_FIXED_DT`. **AE=0.** Restored and
  re-captured: AE=0 again. This is the gate that matters — the draw path is what changed, and neither
  the layout dump (no glyphs) nor the exterior gate (almost no text) can see it.
- 5 cache gtests: a hit returns exactly what shaping would have (metrics and every glyph), wrap width
  and scale are part of the key, frame-aging evicts, atlas growth does not invalidate. Confirmed they
  can fail via a deliberate wrong assertion.
- The wrap-sweep regression tests still green; `checks.default`/`ui`/`cook` green; exterior AE=0; 5/5
  UI layout baselines byte-identical.

## Authoring profile — the finding that should reorder M1–M3 (2026-08-03)

M0 left authoring dominant (3061us vs 1843us of layout at the extreme case), and **no milestone in
this brief touches authoring**. So before scoping M1–M3, the question was: what IS that cost?

A ladder over 2000 nameplates, each rung removing exactly one thing, node count held constant
(`UiBench.AuthoringBreakdown`):

```
computed name (today)          1935us
prebuilt name                  1348us   (-587us  building the string)
literal name                   1367us   (   ~0   unique vs shared string in the arena)
plain child, no text            824us   (-543us  own() + the text table)
raw builder, no facade          640us   (-184us  the facade, same node count)
```

- **~1130us (58%) is string handling** — 587us building `"Player" + std::to_string(i)`, 543us in
  `own()` and the text side-table.
- **640us (33%) is the builder floor** for 4000 nodes. Not the target.
- **184us (10%) is the facade itself** — Element construction, theme, motion. Cheap.
- Unique vs shared strings costs nothing: these are short and stay in SSO, so it is the PER-CALL
  overhead that hurts, not the bytes.

**Both halves point at the same fix**, and it is not in this brief: an arena that can be WRITTEN INTO
directly (bump allocator returning `string_view`, plus a format-into-arena entry point so
`"Player" + to_string(i)` never materialises a temporary). `Ui::own` today constructs a `std::string`
and pushes it onto a `deque<std::string>`, then destroys all of them at `begin_frame` — ~270ns per
call, which is the deque and the object churn, not the copy.

That is worth ~1130us at this scale, against M2's remaining ~337us (see below). It is independent of
M1 and M2 and could land first.

### What M0 did to M2's value

M2's best evidence was the nameplate: a plate that moves 1px re-lays-out AND re-shapes its subtree.
**The re-shaping half is now free** — a moving plate's text, scale and wrap are unchanged, so it is a
cache hit. What M2 still removes is the structural relayout: 337us per 1000 plates, 1025us at the
extreme. Real, but M2 went from removing the dominant cost to removing a second-order one, and it
should be sequenced after M3 rather than before.

### M1 is the same decision as brief 12's canvas

Worth stating plainly since the two briefs reached it separately: brief 12's deferred canvas ("an
ordered LAYER — draw/hit ordering plus an optional modal flag; `(overlay << 8) | z` becomes
`(canvas << 8) | z`") and M1's layers ("generalizes the `element.overlay` boolean into an ordered
list... modal is a LAYER, not a per-element bit") are one concept. Deferring canvas and adopting M1
are not independent choices. M1 bundles it with same-frame popup anchoring and the surface identity
M3 needs, so the value proposition differs — but it is the same mechanism.

### Suggested reordering

1. **Arena + formatting** (not currently a milestone here) — biggest measured win, independent.
2. **Resolve the two-`Ui` question** (open question 1) — blocks M1's registry, and has already caused
   two silent bugs outside this brief (`observe()`, `clipboard_write`).
3. **M1**, then **M3**, then **M2**.

## Frame arena — landed, smaller than predicted (2026-08-03)

`FrameArena` (`string-ui/include/string/ui/frame_arena.hpp`) replaces `Ui`'s `deque<std::string>`
with chunked bump allocation: a copy into the current 64 KiB block and a pointer bump, blocks REUSED
across frames rather than freed. It keeps the non-relocation invariant the deque was chosen for (a
view handed out earlier must survive any later appending) without the per-string object churn.

The largest single gain was NOT the deque swap. `own(std::string_view)` previously did
`own(std::string(s))` — so `Element::text()`, which every text node calls, built a temporary
`std::string` purely to hand it to the arena. That round trip is gone.

Measured (2000 nameplates, authoring only): `literal name` 1367us -> ~1060us (~22%), full computed
path 1935us -> ~1800us (~7%). Run-to-run noise is roughly +/-5%, so the second figure is real but
close to it.

### `Ui::format()` was built, measured, and REMOVED

The plan said the other half of the win was a format-into-arena entry point, so
`"Player" + std::to_string(i)` would never materialise a temporary. Built it. It was **slower**:

| | 2000 plates |
|---|---|
| `own("Player" + std::to_string(i))` | 1801us |
| `format("Player{}", i)`, `formatted_size` + `format_to` | 3335us |
| `format("Player{}", i)`, single pass via `format_to_n` | 2402us |

Two passes over the format string was part of it, and fixing that recovered a third — but even
one-pass `std::format` is ~33% slower than the concatenation it was supposed to replace.
`std::to_string` of a small int plus an `operator+` are both SSO, so the "obvious" temporary is
already close to free, while `std::format`'s parsing and type-erasure machinery is not.

**The API was removed rather than shipped.** Something that looks like the fast path and is 33%
slower is a trap, and a comment warning about it would not have stopped anyone using it.

**So the ~600us of string BUILDING has no cheap fix at the kit level.** It is not formatting overhead
to be optimised away; it is the cost of producing a few thousand unique short strings per frame. The
real answer is to stop producing them per frame — cache a nameplate's text per entity and re-own only
when it changes — and that is an APP-level change, not a kit one. Noted here so the idea is not
re-proposed from first principles.

### Corrections to the profile write-up above

- The first draft of the breakdown ladder compared "no text node" (1 node per plate) against the
  other rungs (2 nodes), so two of its differences measured node count rather than what they claimed.
  Fixed by using a plain child element instead of removing the child; the numbers in the profile
  section are from the corrected ladder.
- I predicted the arena would recover most of the 543us attributed to "own() + the text table". It
  recovered part; the rest is the text side-table and the extra Element, not the allocator.

### Verification

5 arena gtests, the load-bearing one being that views survive crossing block boundaries (20k strings,
asserting more than one block was actually used — confirmed failable). Plus oversized strings, clear
reusing memory, empty strings allocating nothing, and `own` copying rather than aliasing.
`checks.default`/`ui`/`cook` green; exterior AE=0; 5/5 UI baselines byte-identical.

## M1 step 1 — one `Ui` per frame (open question 1, settled) — 2026-08-03

The brief says to settle this FIRST because it decides the seam everything hangs off. It is also the
step that pays for itself immediately, because the two-`Ui` arrangement was not merely duplication.

`DebugPanels` built its own `Ui` over the same builder, with its own `Motion` and its own
`PanelStore`. It now authors into the APP's `Ui`; `begin_frame`/`end_frame` belong to the app, which
owns the frame boundary.

### Two real bugs this fixes, not just a tidy

**Panel z was ranked in two independent stacks.** Panel z comes from `PanelStore::depth_of`, so with
two stores the app's panels and the debug panels each ranked from 1 — brief 14's claim that "a debug
window can now be raised above or buried under a scene panel" was not actually true, because raising
one only reordered it within its own store. Verified from a live dump of `STRING_SCENE=ui
STRING_UI_SCREEN=panels` with the console and log panels open:

```
panel_stats z=1  panel_controls z=2  panel_notes z=3  panel_tiny z=4
dbg_console z=5  dbg_logs z=6
```

One continuous ranking. Before, both sets started at 1 and their relative order was whatever tree
order happened to give.

**"Reset panel layout" cleared only the debug store** while saying "panel layout". It now resets
every panel, which is what it always claimed.

And the class that bit twice — an owner forgetting a per-frame hand-off — is gone by construction:
there is one `observe()`, one `wants_text_capture()`, one `clipboard_write()`, forwarded once in the
app author. `DebugPanels::observe()` is deleted rather than left as a no-op.

Themes were value-identical before the fold (`main.cpp` does `set_theme(client::theme())` and
`client::theme()` is a default-constructed `Theme`), so sharing one changes no colours — which is why
the layout baselines can police this.

### Verification

`checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI layout baselines byte-identical, plus the
live dump above showing the merged z stack.

### What remains in M1

The registry itself: ordered layers, surfaces as recorded node ranges, the declaration/emission split
for inline-declared surfaces (popups), `Ui::resolved_box(id)` for same-frame anchoring, and
layer-aware focus/nav. Those change behaviour on purpose and need re-baselining plus a live pass —
this step deliberately does not, which is why it went first.

## M1 step 2 — layers replace the overlay bit (2026-08-03)

`element.overlay` — one bit, documented as "topmost render LAYER" — is now `element.layer`, an
ordered `ui_layer` enum: `content`, `panels`, `popups`, `modal`.

One bit was never enough, and the gap had been filled with hand-picked z values:

| claimant | was | now |
|---|---|---|
| floating panels | `overlay` + z = stack depth (1..254) | `panels`, z = stack depth |
| lens chrome | `overlay` + `z=200` | `popups`, z=0 |
| dock drop preview | `overlay` + `z=253` | `popups`, z=1 |
| menu bar and popups | `overlay` + `z=255` | `popups`, z=2 |

Four claimants on one byte, correct only by coincidence of the numbers. The drop preview's comment
said "above every panel, below the debug surfaces" — describing a layer that stopped existing when
brief 14 moved the debug surfaces onto the panel stack, so that 253 had been arbitrary ever since.
Lens chrome sat above panels only because no panel stack ever reaches depth 200.

**The batching key is unchanged in shape**: `(layer << 8) | z` where it was `(overlay << 8) | z`, so
the renderer's draw batching and `hit_test_layered` compare exactly what they compared before, and
element `z` keeps its meaning within a layer. Inheritance is now MAX along the ancestry rather than a
sticky bit — a subtree cannot sink below the layer it was placed on, which is what the bit did too.

This is brief 12's deferred "canvas" and the layer half of 12b's M1, which are one concept. Surfaces
— node ranges, per-surface placement and signatures — are the REST of M1 and are not this.

### Verification, and its limit

Relative order is preserved exactly: old effective ordering was content < panels(depth) < lens(200) <
preview(253) < menu(255); new is content < panels(depth) < lens(p0) < preview(p1) < menu(p2). Verified
from a live dump with every claimant on screen at once (app panels, three debug panels, the menu bar
and the lens) — panels came out z 2/3/5/6/8 on the panels layer, menu on popups z=2, lens chrome on
popups z=0.

The 5 UI baselines differ from the previous ones by EXACTLY the dump's field rename
(`overlay=-` -> `layer=-`) and nothing else — checked by normalising that one token and diffing, which
came out identical for all five. They are re-cut on that basis.

**No pixel A/B was possible for this step**: the whole tree is staged-but-uncommitted, so there is no
"before" build to capture against. Exterior AE=0 and `checks.default`/`ui`/`cook` are green, and the
ordering argument above is exact, but the honest statement is that this step rests on reasoning plus a
structural dump rather than on pixels.

**User-verified 2026-08-03**: menu and lens layer correctly over panels.

The drop preview was NOT eyeballed — staging a drag-to-dock by hand is awkward, and it turned out to
be better proved by a test anyway (`WorkspaceTest.DropPreviewDrawsOnThePopupLayerAbovePanels`): the
drag is plain data, so a test can carry a card over a region and assert the emitted tree. It pins
that the preview is the only popup-layer node, that nothing else claims that layer, and that it stays
below the menu bar's z. Confirmed failable by putting the preview back on the panels layer.

Writing it surfaced how the gesture actually works, which is worth recording: **a card drag is
sustained only while its element still holds pointer capture** — the authoring path calls
`end_card_drag` otherwise, so the very next authored frame treats a drag with no capture as a DROP.
A first draft called `update_card_drag` manually and left `interaction::active` at 0; the card was
docked before the assertion ran, and the diagnostic then indexed a stale node and aborted. Driving
capture + cursor and letting the author path resolve the target is both the fix and the honest test.

## M1 step 3 — deferred surfaces; popups anchor to THIS frame (2026-08-03)

A popup is declared while the thing it hangs off is still being authored, so its anchor has no box
yet. That is why `MenuBar` read `observed_box` — LAST frame's box — and the comment defended it with
"the bar does not move". A stale answer being defended rather than a right one: a menu bar inside a
moving panel breaks it immediately.

`Ui::defer(layer, body)` declares a surface to be authored AFTER the main tree has been laid out.
`Ui::emit_next_deferred()` opens the next one as its own OUTERMOST container — so the builder lays it
out independently against the screen, which is what makes it a surface rather than a subtree — and
the host closes it, because only the host holds the measurer. `UIPass` drains in a loop, so a body
may declare another (a submenu over a menu) and still be picked up. Lowest layer first.

`Ui::resolved_box(id)` is then a SAME-FRAME read, and the distinction against `observed_box` is now
explicit in both docs: `observed_box` is one frame stale and works for any element — right for an
EXTENT (a scroll area's content height); `resolved_box` is right for an ANCHOR. Asking the stale one
for an anchor is what made popups lag.

The menu popup is the first consumer. Its body captures everything BY VALUE, because it runs after
the `MenuBar` that declared it has been destroyed — arena views survive the frame, `std::function`
copies.

### Three drafts of the test, and why the first two were worthless

The test moves the anchor and asserts the popup follows within the same frame. Getting it to actually
move the anchor took three tries, and the first two PASSED IDENTICALLY with the stale read — i.e.
proved nothing:

1. Wrapping the bar in an offset parent. `MenuBar` pins itself at `.floating(0, 0)` in ROOT space, so
   the wrapper moved nothing. Caught by running the test against a deliberately reverted
   `observed_box` and seeing it still pass.
2. Widening a label under the shared `Rig`, which lays out with `no_measure` — every label is
   zero-wide there, so buttons sit at fixed offsets whatever they say. Caught by an assertion added
   for exactly this ("the anchor did not actually move — the test proves nothing").
3. Widening a label with a REAL measurer. Discriminates: with the stale read the popup sits at x=52
   while its button is at x=202.

The lesson is the one the sentinel habit exists for — a passing test is evidence only once it has
been seen to fail for the right reason.

The shared `Rig` now drains deferred surfaces exactly as the host does; without it every menu test
would fail for the wrong reason (no popup emitted at all).

### Verification

`checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI baselines byte-identical (popups appear in
none of them). The anchor test confirmed failable against the stale read.

**User-verified 2026-08-03**: the menu works — popup lands under its button and its items fire, now
that it emits on a separate pass.
