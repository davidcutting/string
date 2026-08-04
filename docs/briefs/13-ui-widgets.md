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
- **M4c — Text wrap (DONE + USER-VERIFIED LIVE 2026-08-01).** The last of the five. Opt-in via `.wrap()`, so no existing
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

  **Two defects shipped in the first cut, both found by live verify, both now fixed. They are worth
  keeping because each was something this brief had explicitly reasoned its way past:**

  1. **The break decision was per-GLYPH.** `word_w == 0` tests only a word's first character, so if
     "b" fits and "baz" does not, no break is taken — and by the second character the word is no
     longer at a break opportunity, so its tail overflows and is clipped. Character wrap wearing word
     wrap's clothes: correct at most widths, wrong at exactly those where a word's first letter fits
     and its tail does not. The measurer and the shaper had it *identically*, so line counts agreed
     and only the visual was wrong. Fixed with a shared `measure_word()` both call.
     **This corrects the note above about Clay's per-word measurement being an optimisation we did not
     need: the LOOKAHEAD is what makes the decision correct. Only the CACHE is optional.**
  2. **A wrapped line's width included its trailing space** — Clay's `finalCharIsSpace` trim (2616),
     dismissed here as cosmetic. It is not: a line broken after a space measured one space *wider*
     than the box it had just been wrapped into, so the element reported a preferred width its own
     wrap width could not satisfy. Now tracked as `visible` (the pen up to the last drawn glyph) and
     recorded at wrap breaks only; explicit newlines and end-of-run keep the pre-wrap accounting,
     because that path predates wrapping and every existing screen is measured through it.

  **Method note, for briefs 14/15: the regression test SWEEPS every wrap width** from the floor to
  600px rather than checking one. A single width would have passed on both bugs — only 72 and 73
  failed, off by exactly one space. The invariant is "no line may be wider than the wrap width", and
  `string-engine/test/text_wrap_test.cpp` checks it against a real font atlas along with three
  others (min_width really is the widest word, line count is monotone in width, an over-long word
  still hard-breaks).
- **M5** — Immediate-mode CPU cost reduction to target.

## Verification (per `README.md` rules)

- Every widget: hover/focus/drag/keyboard + gamepad focus nav correct; handle binding round-trips
  (edit → store → re-read) with no dangling; theme inherit + per-element override both apply.
- Determinism: same inputs → same layout/shaping (whole-pixel advances, per brief-05 rules).
- Immediate-mode CPU cost hits target in the 500-nameplate + debug-panel-heavy scenes (Tracy).
- **NEEDS VISUAL VERIFY** (widget feel, color picker, tables/trees, image widget correctness).

---

## Two architectural fixes from the kit review (2026-08-03)

Both came out of a read-through of the whole kit rather than a bug report. Recorded here because
each closes a class of defect, not an instance.

### Focusable is now declared, not inferred from having an id

`nearest_focusable` treated every node with `id.hash != 0` as a gamepad nav target. But an id is
what ANYTHING addressable needs — hover, drag, pointer capture, motion state — so panel title bars,
resize grips, scroll tracks, splitters and lens chrome all carry one. Pushing the stick in a
panel-heavy screen landed focus on a resize grip: an element with nothing a stick can do to it.

The tree has three states to express, and only had two:

| | means |
|---|---|
| no id | decoration; the hit test resolves through it to an id'd ancestor |
| id, not focusable | POINTER-DRIVEN chrome. Hover/drag/capture work; nav skips it |
| id + focusable | a control: nav reaches it, the south button activates it |

So `element::focusable` joins `wrap`/`clip`/`wheel` as an opt-in capability. `Ui::button()`, every
widget factory, and `Element::on_click()` set it — registering an activation handler IS a
declaration of focusability, so the common path never says it twice. Only hand-rolled elements that
poll `clicked()` state it explicitly (four such sites: workspace tabs, DAG nodes, resource rows,
the lens close button).

Two further rules fell out of the same distinction:

- **Clicking non-focusable chrome CLEARS focus** rather than parking it there. Otherwise the next
  stick push is measured from a resize grip, which is the same bug arriving by a different route.
  A click on chrome still does not raise the game-mode request — that is for clicks on empty space.
- **Zero-area nodes are skipped by nav.** Collapsed content and empty rows produce them, and focus
  landing somewhere invisible is a trap with nothing on screen to point at.

Gated: 5 new interaction tests + 2 facade tests, all UI baselines byte-identical, exterior AE=0.

### Text shaping is one algorithm, not two kept in step by hand

`dynamic_text_measurer::shape` (string-ui) decided how big a text element's box is; `append_glyphs`
(string-render-forward) decided where each glyph goes. Both walked UTF-8, applied kerning, rounded
advances to whole pixels and made the same two word-wrap break decisions — in different libraries,
each carrying a comment warning that it MUST match the other or text would draw more lines than its
box reserved and overdraw whatever sat below. A correctness hazard maintained by discipline across a
library boundary is the kind that survives review and fails in the field.

Now: `string::shape_text` (string-ui/include/string/ui/text_shaper.hpp) walks the run once, returns
`{width, lines}`, and invokes a sink per drawable glyph. The measurer passes `no_glyphs{}`; the
renderer passes a lambda that translates to screen space, clips to the node box and pushes a quad.
`measure_word` moved with it. Nothing else changed — the break rules, the trailing-space trim and
the (preserved, documented) quirk that kerning carries across a wrap break are all as they were.

**The pixel gate earned its keep here.** The first draft snapped glyph positions inside the shaper,
reasoning that rounding commutes with adding an integer box origin. It does not: `std::round` rounds
halves AWAY FROM ZERO, so `round(5 + -0.5) == 5` while `5 + round(-0.5) == 4`. A glyph's left side
bearing is frequently negative, and the FIRST glyph of a run is the one whose local pen is 0 — so
the leading character of a great many runs moved by one pixel. The layout dump could not see it
(measurement was unaffected) and the unit tests could not see it (line counts were unaffected); an
A/B pixel capture of the UI scene showed it immediately, as red on the first letter of every
affected word.

The rule that came out of it, now documented in the header: **advances are rounded by the shaper,
positions are not.** Whole-pixel advances are a property of the run and measurement depends on them.
Pixel-snapping a position is a rasterisation concern of whoever knows the screen coordinate, and
must happen after the translation.

Verified by restoring the old hand-written shaper behind a temporary `#define`, capturing the UI
scene under `STRING_FIXED_DT` (run-to-run AE=0 first, to prove the scene is deterministic at all),
then swapping in the shared one: **AE=0**. Plus 2 new tests — placement never uses more lines than
measurement reserved (swept across 360 widths), and shaping with and without a glyph sink agrees.

Deliberately NOT asserted: that glyph quads fit the measured box. A measured box comes from advances
and line heights; a quad is the atlas bitmap, which for SDF carries spread padding on every side, so
it legitimately spills a pixel or two — as overhangs and italic tails do in any text stack. Pinning
that would pin a bug, not a property. (Both axes of a first-draft test asserted exactly that and
failed; the tests were corrected rather than the tolerance widened.)

## Kit review cleanups, round 2 (2026-08-03)

### One hit test, not two

`layout_builder::hit_test` is deleted. It picked the last node in pre-order containing the point —
painter order WITHIN a layer only — and knew nothing about `clip`. So it clicked through floating
panels and it hit content that had been scrolled out of view, while `ui::hit_test_layered`, which
handles both, sat right next to it. Two hit tests with different answers is one hit test and a trap;
the only thing keeping the wrong one alive was that tests called it.

`hit_test_layered` is now THE hit test, and `layout_builder` is layout-only. The consteval self-test
and three unit tests moved across; the one test that asserted "the plain hit test does NOT handle
overlay" is gone with the function it was documenting.

### State -> colour is a theme question, answered once

Thirteen hand-written ternaries across the widgets decided what hover, focus and selection look
like — so "change the hover feel" meant finding all thirteen and getting each right, and any new
widget had a fourteenth chance to invent its own. `Theme` now carries the mapping, taking a
`visual_state { hot, active, focused, selected }`:

- `surface(s)` — fill for a selectable/hoverable row, option or header. **Selection outranks hover**,
  so a selected row stays legible as selected while you sweep over it.
- `outline(s)` — border for an interactive control. **Focus outranks hover**, because focus persists
  and is what the keyboard acts on; it has to stay readable while the pointer is elsewhere.

Eleven sites migrated: checkbox, drag value, slider, text field, combo head + option, collapsible
head, table row, tree row. `DragValue`'s `active ? panel_alt : (hot ? panel_alt : panel)` turned out
to be `surface({.hot, .active})` written awkwardly.

**Only the already-unanimous mappings were absorbed.** Four sites deviate and were left alone rather
than bent into a shared function: a menu row goes to `accent` on hover, a menu-bar button rests
transparent, a tab keeps its resting fill and signals selection through its stroke, and `button()`
has its own press/focus treatment. Forcing those through `surface()` would have changed how they
look, which is a design decision and not a cleanup.

**A real inconsistency this surfaced, deliberately NOT fixed:** hover moves in opposite directions
depending on the widget. Table rows, tree rows, combo options and collapsible headers go from
`panel` to `panel_alt` (darker); graph nodes and tabs go from `panel_alt` to `panel` (lighter). One
of those is wrong, but which one is a visual call, and changing either churns pixels — flagged for a
decision rather than silently normalised.

Gated: all 5 UI baselines byte-identical (so the RESTING appearance is provably unchanged), exterior
AE=0, `checks.default`/`ui`/`cook` green, plus 3 new tests. The baselines matter less than usual
here: a layout dump only ever captures a resting frame, so hover and selection are exactly what it
cannot see. Hence the third test, which drives a table with a hovered row and a hovered-AND-selected
row and asserts the fills — the function being right is not the same as the widgets being wired to it.

### The third cleanup did not survive inspection

The plan also listed "derived ids go through string concat + rehash (`base + ".move"`) when
hash-combining is already the pattern next door (`slot(id, k)`)". That is wrong, and the reason is
worth writing down so it does not get re-proposed.

`slot()` can be numeric because widget-state keys never enter the tree. Element ids DO: `id.name` is
printed by `layout_dump`, which is what the five UI baselines diff, and what makes a dump readable
at all (`id=lens0.frame#8cd5dd69` rather than `id=-#8cd5dd69`). Hash-combining derived ids would
churn every baseline and take the names off the one tool used to debug layout.

The cost that motivated it is also smaller than claimed: these names are short, so `std::string`
stays in SSO and the deque allocates in amortised blocks. What remains is a concatenation temporary
and an fnv1a over ~12 bytes, per element per frame, at ~172 nodes.

A follow-up idea — a compile-time-string overload, so literals skip the arena entirely — was then
MEASURED and dropped. The mechanism is easy (an `element(id)` overload beside `element(string_view)`;
the `_id` consteval literal already exists, and the hash would fold at compile time). It just helps
nothing here:

| | |
|---|---|
| element names in the busiest screen | 32, ALL computed (`inv_0..inv_23`, `act_0..act_7`) — zero literals |
| named nodes on the screen that scales | 0 (nameplates is text only) |
| text runs across all 5 screens | 158, mean 6.6 chars, 1042 bytes total |
| of those, SSO (no malloc) | 143 / 158 |
| the 15 that DO heap-allocate | longest are `"anchors: 24   nameplates: 24"` — computed |

The population such an overload can serve is literal, static-lifetime strings; in this codebase those
are precisely the short ones already living in SSO and never touching the allocator. Everything
genuinely expensive is computed at runtime and therefore ineligible by construction. Total arena
traffic is about a kilobyte of memcpy and ~10 amortised deque allocations per frame.

If the arena ever does show up in a profile, the fix is the opposite shape: replace
`deque<std::string>` with a bump allocator returning `string_view`s. That removes the per-string
object altogether and helps the COMPUTED strings — the actual population, and the only one that grows
with entity count.

## TextField: real text editing (2026-08-03)

`TextField` was append-and-backspace-from-the-end. That is fine for a debug console and not fine for
chat, which is the first real consumer. It now has a caret, selection, word motion and the clipboard.

### The constraint that shaped the rendering

**The kit owns no font atlas** — measurement belongs to the host, which passes a measurer to
`builder.end()`. So a widget cannot ask how wide "hell" is in order to draw a caret after it.

The caret and selection are therefore drawn by **splitting the run into separate text nodes** and
letting LAYOUT position them: `[before][caret][after]`, or with a selection, the selected run nested
in a filled container (text nodes draw glyphs, not rects, so the highlight has to be a parent). No
measurement, no font access, no new dependency.

The cost, accepted and documented: kerning is not applied across a split boundary, so a caret sitting
between two kerned glyphs shifts the tail by a fraction of a pixel. Invisible at UI sizes, and the
alternative is dragging font metrics into a kit whose whole value is depending on nothing.

### The model is separate from the widget

`text_edit.{hpp,cpp}` — `apply_text_edit(text, state, keys)` is pure logic over a string. Editing is
the part of a text field with all the edge cases (UTF-8 boundaries, word jumps, what backspace does
to a selection, what typing over one means), and it is the part that has nothing to do with layout.
Splitting it out makes every rule directly unit-testable, which the widget's emit-into-a-tree shape
is not. 18 tests live there; the widget gets 5 more for the wiring and the tree it produces.

Rules worth keeping (each is a test):

- Offsets are BYTES but always sit on codepoint boundaries. `clamp()` enforces it, and is called
  every frame because the bound value can be written by something other than this widget.
- Both delete keys remove the SELECTION when there is one. Treating backspace as "one character
  back" while text is highlighted loses the selection silently.
- Plain Left/Right with a selection **collapses to the near edge** rather than stepping from the
  caret. Stepping makes the far end jump by one and reads wrong.
- Copy/cut with no selection are NO-OPS, not "operate on the whole field" — a surprising way to lose
  a line.
- Within one frame: select-all, then clipboard, then delete, then movement, then typed text. Two of
  those orderings were found by tests rather than reasoned out — select-all-before-copy (a frame
  carrying both copied nothing) and move-before-type (a character landed where the caret used to be).
- **Gaining focus puts the caret at the end.** Without it, a field opened on an existing value
  prepends the first character typed and backspace does nothing — both read as a broken field.

### Platform additions

Neither existed: **key auto-repeat** (`Input::key_repeat` / `key_edit`, fed from SDL's repeat flag —
the delay and rate are an OS accessibility setting and a UI must not invent its own) and the
**clipboard**. The clipboard is cached and refreshed on SDL's clipboard-update event rather than
polled, because `SDL_GetClipboardText` allocates on every call for a value that changes almost never.

Both crossings reuse established patterns rather than adding new ones: editing keys and the clipboard
arrive on `interaction_input` (the host resolves which chord means copy, since that is a platform
convention — Cmd on macOS), and a copy leaves as a `Ui::request_clipboard_write` handled exactly like
`request_text_capture`. Editing keys are deliberately NOT actions, for brief 17's reason: nobody
rebinds Home.

### Verified

23 new tests. Execution confirmed by breaking an assertion and watching that specific test fail.
`checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI baselines byte-identical — the baselines
prove nothing about this feature, since they capture an unfocused frame and a caret only exists while
focused, which is exactly why the widget-level tests assert the emitted tree instead.

### Still missing

- **Click to position the caret**, and double-click to select a word. Both need pixel → byte offset,
  which needs measurement — the one thing the split-node trick cannot supply. It wants a measurement
  seam for widgets, which is a real architectural addition rather than a widget change.
- **Scrolling within the field** when the text is wider than the box. It clips today.
- Single-line only; no multi-line editing.

### The console was never using it (user, 2026-08-03)

"Arrows don't move the cursor and there is no shift selection." Correct, and nothing to do with the
widget: **the debug console was not a `TextField`.** It was a plain text element with a `"_"` glued
on the end, backed by a second hand-rolled editor in `handle_console_input` — including a byte-wise
`input_.pop_back()` backspace, the very UTF-8 bug `TextField` had already fixed. My suggestion to
"type into the console" to test the new feature was therefore wrong.

The console's edit line is now a real `TextField`, and `handle_console_input` keeps only what is
console semantics rather than text editing: history, completion, and running the line. A second
implementation of a shared problem drifts to the worse one; it took a bug report about a feature that
was working to notice this one had been sitting there.

Two things the migration needed:

- **`TextField::focus(bool)`** — a modal that owns a field must be able to make it live, and there is
  nothing to click. Deliberately an OWNER ASSERTION rather than a way to push focus through
  `interaction`: focus is resolved post-layout from the tree, so a programmatic write would be a
  second source of truth for the same thing. Two fields both forcing focus is an authoring error of
  the same class as two elements sharing a name. Chat will want exactly this.
- **Snap the caret to the end when the value is replaced from OUTSIDE.** Tab-completion and history
  write the bound string directly, and a caret left at its old offset then sits mid-text. `clamp`
  cannot catch it: the string usually gets longer, so the stale offset stays valid while being wrong.
  Detected by hashing the value (widget_state holds scalars, not strings); a collision costs one
  missed snap, degrading to the old behaviour rather than to anything broken. The hash is recorded
  AFTER the widget's own edits, so only an external write registers.

**A test-rig bug found on the way, worth keeping:** `Rig::frame` cleared `scroll_y` but not the other
per-frame events, so a caret key set for one frame was still set during the next frame's typing —
making a correct widget look broken in a test. It now clears typed text, the caret keys and the
clipboard chords too, and one older test that had been relying on sticky input was corrected to
re-supply it per frame.

### Copy did not reach the clipboard (user, 2026-08-03)

"ctrl+c/v doesn't work." Copy was definitely broken; paste was fragile.

**Copy — the same defect class, for the second time.** The console's field lives in DebugPanels' OWN
`Ui`, and only the APP's `Ui` had `clipboard_write()` forwarded to the host. The request went
nowhere, silently. This is exactly the shape of the `observe()` miss that broke the scroll extent:
an object that owns a second `Ui` also owns its hand-offs, and forgetting one produces no error, no
log, and a feature that simply does nothing. DebugPanels now forwards its own.

**Two instances of this class now.** A guard was considered after the first and declined as
speculative — reasonably, on one data point. It is no longer one data point: any `Ui` whose owner
forgets a hand-off fails silently, and the hand-offs are growing (`observe`, `clipboard_write`,
and `wants_text_capture` which DebugPanels happens to satisfy by another route). Worth revisiting.

**Paste — the cache was too clever.** The clipboard was read once at startup and refreshed on SDL's
`CLIPBOARD_UPDATE`, to avoid an allocating syscall per frame. But that event can be missed while the
window is unfocused, which is the common flow exactly: copy in another app, alt-tab back, paste. It
now also re-reads on `WINDOW_FOCUS_GAINED` — one syscall per focus change, and no dependence on an
event whose arrival we cannot verify.

### The toggling backtick was typed into the console (user, 2026-08-03)

Dropped in the migration: the old `handle_console_input` filtered `` ` `` and `~` out of every
keystroke, and once TextField took over typing, nothing did.

Fixed by **swallowing the keystroke that toggled**, not by banning the character: on the single frame
the grave key opens the console, the field is authored unfocused, so that frame's text event reaches
nothing. Precise — the keypress is consumed by the toggle it performed, and a backtick remains a
character you can type afterwards, where the old filter banned it permanently to solve one frame.

The flag is cleared at the END of `update_and_author`, after the field has been authored. A first
draft cleared it on a key-state condition instead, which left the field unfocused for as long as the
console stayed open — the console would have opened and then ignored everything.
