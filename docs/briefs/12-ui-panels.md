# Brief 12 — UI panel system + docking

Status: **COMPLETE 2026-08-01** — M0 (facade) + M1 (floating panels) + M2a-d (workspace, docking) all
user-verified live; M2e (persistence) and M4 (retained seam) DEFERRED with reasons recorded below.
**UI API conventions locked with user 2026-07-25**
(shared by briefs 13–15). **Engine/sandbox layering + M0 refined with user 2026-07-28** (see below).
Part of the tooling/UI arc (11–15) before 08 VFX. Deps: brief 11 is COMPLETE (M0–M3 + naming +
M4-introspection; the M4 mutable store is deferred INTO brief 15). Brief 13 (widgets) builds on this.

## Goal

A full **ImGui-like dockable / splittable / tabbed panel system with layout persistence**, and —
because this is the first UI brief of the arc — it **establishes the matured UI authoring
abstraction** (naming, events, binding, nesting, sizing, theming) that briefs 13–15 all build on.

This is an **evolution of brief 05**, not a rewrite: keep the `layout_builder` immediate-mode core,
the motion table, the dynamic SDF atlas, and gamepad focus nav. We put a clean fluent facade on
top and add real panel management.

Inspiration: **Clay** (declarative element tree, sizing model, explicit ids) — but drop the
C-macro ergonomics for typed modern-C++ builders.

## UI API conventions — LOCKED with user (2026-07-25)

These apply to **every** element (panels included) across briefs 12–15.

- **Explicit names as identity** (like pass debug names — self-documenting). Every element takes
  an explicit name string; it is the *stable id* used to persist transient interaction state
  (hover, focus, active-drag, text-cursor, motion/animation) across the per-frame immediate-mode
  rebuild. The name is **independent of the visible label** (label defaults to the name; override
  with `.label("…")`). This is the fix for the brief-05 silent-id-collision class of bug.
- **Callback-first events** (`.on_click(fn)`, `.on_change(fn)`), with a poll convenience
  (`.clicked()`, `.changed()`) as *sugar over the callback primitive* for throwaway immediate-mode
  debug UI. Rationale: polling is immediate-mode-only (needs the call site re-run each frame);
  **callbacks are the superset that a future retained layer requires** (see below).
- **Data binding = a lightweight typed accessor** (`bind(...)`), not raw pointers as the norm.
  Two flavors: a **handle-backed accessor** (CVar handle, or the brief-11 material/light store
  handle) = the retained-safe canonical path; and a **raw obj+member accessor**
  (`bind(obj, &T::field)`) = the **immediate-only, within-frame escape hatch** (safe because it
  never outlives the build). **Rule: retained binds are restricted to handle-backed targets** —
  raw-ref binds are legal only on the immediate path. App state that must be retained-editable is
  pushed behind a store or CVar (the price of retained binding — a *rule*, not mirroring code).
  **Type erasure is needed only for the persistent retained tree**, so it lands WHEN retained mode
  is actually built; the immediate implementation is just a typed accessor consumed the same frame
  (deliberately minimal). Kills the brief-05 dangling-view bug class. Can grow a getter/setter
  closure variant later if computed/validated properties are genuinely needed (additive, not a
  teardown).
- **Closures for nesting** (`.content([&](Ui& u){ … })`) — trees read naturally; chaining is for
  per-element config.
- **Full Clay sizing vocabulary** (grow / fit / fixed / percent, direction, gap, pad, align) **with
  sensible inherited defaults**, so the simple case stays terse and the powerful case is reachable.
- **Central theme all elements inherit from, with per-element override** (user: theming is a must).
  One style reference in one place (colors, spacing, radii, font sizes, etc.); elements override via
  chaining (`.color(…)`). A scoped style push/pop stack is optional-later if it earns it.

```cpp
u.panel("render_debug").label("Render Debug").dock(Dock::Right).size(320).content([&](Ui& u){
    u.slider("sun_intensity", bind(sun, &Sun::intensity)).label("Intensity").range(0, 10)
                                                          .on_change([&](float){ mark_dirty(); });
    u.button("reset_mat").label("Reset").on_click([&]{ reset(sel); });
});
```

### Retained-mode constraint (design for it now)

The user may layer a **retained-mode UI on top of the immediate core** later. The three primitives
that make retained-on-immediate work are already chosen: **explicit stable names** (persistent
identity), **callbacks** (events without a re-run call site), and **handle bindings** (persistent
data). Immediate mode stays the core; the panel/widget code must not assume the build re-runs every
frame in a way that blocks a retained layer being added. This is a real architectural constraint on
this brief, not an afterthought.

## Panel-specific scope

> **Superseded in part by "The model" below (2026-08-01), which
> is the authority. In particular "panel" now means a TAB GROUP (docked or floating), "card" the
> content unit, and persistence is no longer versioned. Kept for the scope it still fixes.**

- Dockable / splittable arrangement: drag-to-dock, split regions (h/v), resize handles, z-order.
- Tab bars (multiple cards sharing a slot).
- **Layout persistence**: serialize/restore the arrangement so a debug layout survives restart.
- Cards and panels are elements too — authored with the conventions above.
- Engine (`string::` UI) owns the generic workspace/panel machinery; sandbox authors the
  concrete cards (debug cards land in briefs 14/15).

## Engine / sandbox layering — DECIDED with user (2026-07-28)

The layout core is **already engine-side** (`string/core/layout.hpp`: element tree, `sizing`/
`size_mode`/`direction`/`alignment`, `id`+`make_id`, `hit_test`/`find`), as is the text stack
(`dynamic_font`, `text_measurer`) and `platform::Input`. So this brief is a facade-and-panels job,
not a UI rewrite. Eight strata, by owner:

| Layer | Owner | Contents | Status |
| --- | --- | --- | --- |
| L7 screens / concrete panels | sandbox | `screens.*`, `ScreenState`, `debug_panels.*`, `UiScene`, palette instance | exists |
| L6 widget kit | **split** | engine: `button`/`progress_bar`/`tooltip` · sandbox: `icon_cell` | brief 13 |
| L5 panels | engine | workspace (splits + panels), splitters, tab bars, z-order, persistence | NEW |
| L4 fluent `Ui` facade | engine | names, callbacks, `bind`, `.content()`, sizing + the string scratch arena | NEW |
| L3 theme | engine (type) / sandbox (values) | inheritable `Theme` struct + per-element override | refactor |
| L2 motion | engine | `Motion`, `Curve`, `Transition` | clean lift (~190 lines) |
| L1 interaction | engine | hover/focus/press, gamepad nav, capture mode + NEW drag/pointer-capture/z-ordered hit-test | extract + extend |
| L0 layout + text | engine | `layout_builder`, sizing vocabulary, ids, hit-test, fonts, input | DONE already |

**Widget line (principle, implemented in 13): engine widgets encode _interaction_, sandbox widgets
encode _meaning_.** `icon_cell`'s rarity border + radial cooldown sweep is meaning.

### The dependency inversion (L1) — the decision that gates the rest

Engine panel code cannot compile against interaction state a sandbox class owns. Today `UIPass`
holds `hovered_id_`/`focused_id_`/`prev_click_`/`stick_armed_` and does hit-test, focus arbitration
and gamepad focus-nav itself (`sandbox/passes/ui_pass.cpp:507-590`, ~80 lines). Docking needs
strictly more: **active-drag identity + origin/delta, pointer capture, z-ordered hit-testing** —
none of which exists.

Three options were considered: (1) promote `UIPass` wholesale; (2) promote the *state*, keep the
*producer* in sandbox; (3) engine owns a `ui::host` running layout+interaction, the pass becomes a
thin renderer. **DECIDED: option (2) now, with (3) as the shape it grows into.** (1) drags shader
ownership and bindless wiring across mid-brief; (3) is the principled end state but a bigger step
than this brief should take.

**This is a deliberate deferral, so the constraints that keep (3) cheap are LOAD-BEARING, not
nice-to-haves:**

- **`ui::interaction` is a plain VALUE TYPE** — state in, no behaviour, no back-pointer to the pass,
  no callback into it. The failure mode that turns (3) from a move into a rewrite is this struct
  quietly growing a `UIPass&`. Value-typed also makes L1 unit-testable without a GPU.
- **Exactly ONE crossing function.** The sandbox pass fills it via a single
  `populate_interaction(...)`; that is the *only* place sandbox touches engine UI state. Option (3)
  then becomes "move this function into `ui::host`".
- **Loose coupling + unit-testability is a first-class goal of this brief**, not a by-product: every
  engine layer L1-L5 must be constructible and testable without a device (the L0 layout core and
  `DebugConsole` already prove the pattern).

### Rationale — why not design more up-front (user, 2026-07-28)

Brief 11's shape wasn't knowable until we were in the weeds; the refactor cost was real but
affordable **because a mechanical gate (`AE=0` + sync-validation) made bold moves cheap to verify**
— the VUID-09600 defect proved it twice (byte-parity stayed clean; only validation caught it).

The UI has **no equivalent gate** — brief-05 screen parity is *visual*, so every iteration costs the
user's eyes. That does not argue for more up-front design (it still won't converge until we build);
it argues for **buying the gate first**. Hence M0a below.

## Milestones (refined 2026-07-28)

- **M0a — the gate, before any migration.** A **layout-tree dump**: element ids, resolved rects,
  sizing resolution, parent/child structure, serialised as stable text and diffable across the
  facade migration. This is the `AE=0`-shaped check for L0-L4: it catches silently-changed sizing
  resolution or id assignment — the bug class that is murder to spot by eye and trivial to spot in a
  diff. It does NOT cover motion/docking feel (those stay eyes-on). Capture dumps of the brief-05
  screens BEFORE touching anything; they are the baseline for M0b-M1.
- **M0b — L1 + L2 promotion (mechanical).** `Motion`/`Curve`/`Transition` lifted to
  `string/ui/motion.hpp` verbatim. `ui::interaction` extracted as a value type + the single
  `populate_interaction` crossing; drag/pointer-capture/z-ordered hit-test added. Unit tests for
  both (no device). Tree dumps unchanged vs M0a baseline.
- **M0c — L3 + L4.** Theme constants → inheritable `Theme` struct (values stay sandbox). Fluent `Ui`
  facade over `layout_builder` implementing the locked conventions, with the string scratch arena
  folded in so no author re-derives the deque-not-vector rule. `bind()` built on **CVar handles**
  (the brief-11 material/light store is deferred to brief 15 and slots into the same seam — verify
  the accessor is handle-generic, not CVar-shaped). Brief-05 screens re-expressed through it; tree
  dumps unchanged.
- **M1** — Panel container: float/dock, resize, z-order.
  - **M1a — floating move + resize. DONE (2026-08-01).** `string/ui/panel.hpp` (`panel_rect`,
    `panel_limits`, `panel_state`, `PanelStore`, pure `update_panel`) + `Ui::window(name)` emitting
    a floating overlay container, a title bar (move handle) and a corner grip (resize handle) — both
    ordinary id'd elements, so they hover/focus/capture through the same path as any button. Rect
    state lives in `PanelStore` because the layout tree is rebuilt every frame; that is the only
    reason the store exists, and is independent of any serialisation question. Sandbox demo =
    `STRING_UI_SCREEN=panels`, deliberately NOT in `all` so the five M0a baselines stay byte-equal.
  - **M1b — z-order. DONE (2026-08-01), user-verified.** Click-to-raise, including on widgets nested
    inside a panel. Note what it did and did NOT fix: paint and hit-test ALREADY agreed (both
    followed author order), so nothing was broken — z-order buys the single behaviour "click a
    partly-covered panel and it comes forward".
    The real defect it forced out was in the renderer: `ui_pass` issued four draws (main shapes →
    main text → overlay shapes → overlay text), so **all** overlay shapes preceded **all** overlay
    text and a back panel's TEXT drew over a front panel's BACKGROUND. Click-to-raise only looks
    right if draw order follows z, so batching became one shapes-then-text pair PER LAYER — which is
    exactly what kills the bleed. `string::element` gained `uint8_t z` (inherited like `overlay`,
    ignored by layout, a pure draw-batching key); `hit_test_layered` compares the same
    `(overlay << 8) | z` key, which is mandatory rather than optional once author order stops
    deciding what is on top. Costs nothing when unused: z defaults to 0, so a panel-free UI produces
    the same two batches as before. Debug surfaces sit at a reserved high z so raising a panel can
    never bury them.
- **M2** — The workspace: splitting, tab bars, drag-to-dock. **Designed with the user 2026-08-01;
  see "The model" below — those decisions are load-bearing.**
  - **M2a — DONE.** Weighted `GROW`, plus — after it proved insufficient — `size_mode::PERCENT`.
    **The lesson is worth keeping: GROW starts a child at its CONTENT size and shares only the
    SURPLUS, so pixels → ratio → pixels does not round-trip and a splitter built on it jumps the
    moment it is touched.** `percent` ignores content and round-trips exactly. Splits use percent;
    weighted grow remains for the general case, which is not a splitter.
  - **M2b — DONE.** `Workspace` (forest) + the operation set + the collapse invariants. Pure tree
    code, no device, gtested. Canvas is NOT part of M2 — see "LATER: Canvas".
  - **M2c — DONE, user-verified.** `u.workspace(ws)`: emit, splitter drag, tab selection. Needed a
    post-layout seam (`Workspace::observe`, called from a new `UIPass::PostLayout` hook) because a
    splitter must convert pixels into a sizing and container sizes do not exist at authoring time.
  - **M2d — DONE, user-verified.** Drag-to-dock, tear-off, drop preview, floating panel emit.
    **Tear-off, drag-to-dock and reorder are ONE code path** — the forest model's payoff.
  - **M2e — DEFERRED (user, 2026-08-01), not cancelled.** `save`/`load`, absorbed from the old M3.
    The only consequence is that the workspace resets to its seed on restart, which costs one
    re-arrange. Its value stays speculative until briefs 14/15 provide panels worth arranging — and
    by then it is still a short function, because the data is already serialisable: ids, directions,
    sizings, tab indices, floating rects, and nothing else. Revisit when re-arranging actually
    annoys, not before.

  **VERIFICATION FINDING, load-bearing for briefs 13–15: seven live defects were found across M2c
  and M2d, and the layout-dump gate caught NONE of them.** Every one was interaction-over-time
  (splitter frozen at unit weights, jump-on-click, compress-past-minimum, pick-up threshold, torn-off
  panels at 0x0, unmovable floating panels, nested-split overflow) or state the dump does not render.
  The dump remains the right gate for what it covers — it caught a real emission-order bug in M2c and
  held every migration to zero diff — but **for gesture work the user's live pass is the only gate**,
  and that should be budgeted for rather than discovered.
- ~~**M3** — Layout persistence.~~ **DELETED 2026-08-01, folded into M2e.** Not a descope of the
  feature — the design made it small. A workspace is ids, directions, ratios and tab indices, so
  serialising it is a short function rather than a milestone, and the "versioned so a schema change
  degrades gracefully" language was over-specified for what a debug layout is worth: on ANY parse
  failure, discard and use the seed. Losing a debug layout costs one re-arrange.
- ~~**M4** — Retained-layer seam validated by a throwaway wrapper.~~ **DEFERRED after re-examination
  (2026-08-01), which the entry itself called for once M2 landed.**

  **The workspace already IS the seam, built for real rather than as a prop.** It is a structure that
  persists across frames, carries stable identity, is edited by gestures rather than re-authored, and
  EXPANDS INTO the immediate tree each frame — which is precisely the retained-on-immediate shape M4
  set out to prove is reachable. A throwaway wrapper would now demonstrate less than the shipped
  thing does.

  What the workspace does NOT exercise is handle-backed bindings driving a retained tree, and
  callbacks firing without a re-run call site. Those are worth validating — but by building retained
  mode when it is wanted, not by a disposable wrapper whose findings would be discarded with it.

  The three primitives the retained constraint asked for (stable names, callbacks, handle bindings)
  are all in place and unchanged, so nothing here is blocked later.

## The model — DESIGNED with the user (2026-08-01)

### Start here: most UI needs none of this

**Compose elements by default.** Tooltips, drag previews, main menus, the HUD, nameplates, the
action bar — all of it is plain element composition, exactly as brief 05 already does. None of them
touch panel machinery.

The decision rule is one question: **does the user get to move, dock or tab this?**

- **No** → compose elements.
- **Yes** → it is a card in a workspace.

A card's *interior* is composed elements too. The only thing being a card buys is user-arrangeable
placement. So the panel system is OPT-IN and initially barely used: the debug layer of briefs 14/15,
plus whichever HUD pieces are worth arranging (chat, party frames, minimap). Docking is a TOOLING
feature that happens to also serve a few HUD pieces; it must not sit in the path of the ~95% of UI
that is just composed elements.

### Vocabulary

| term | what it is | persistent |
| --- | --- | --- |
| **Workspace** | the arrangement: a tree of splits whose leaves are panels, plus N floating panels (a FOREST) | yes, serialised |
| **Panel** | **a tab group of cards**; carries its own `axis_sizing`; docked in the tree or floating with a rect + z | rect/z, floating only |
| **Card** | the content unit; interior authored in code; its title is its tab label | no — rebuilt each frame |
| **split** | direction + two children + `locked` (is this splitter draggable). Internal to the workspace | part of the workspace |
| **placement** | where a card goes: `left(fixed(280))`, `right_of(Chat)`, `tab_with(Chat)` | no — a value |

`dock`, `undock` and `close` are **verbs** on `Workspace`. Nothing else is a noun — an earlier draft
had a `dock_target` type, which reintroduced "dock" as a thing after we had deliberately removed it.

### A PANEL IS A TAB GROUP

That is the whole definition, and **only panels have tabs**. An earlier draft had "tab group" as a
leaf type of the docked tree AND "panel" as a separate floating thing — a distinction nobody looking
at the screen would make, since a docked box with tabs and a floating box with tabs are the same
object. **What differs is only chrome**: a floating panel has a title bar, a resize grip and a rect;
a docked one fills its slot and shows just a tab bar.

Consequences, all of them simplifications:

- **The workspace has exactly two node kinds: `split` and `panel`.** Nothing else.
- **A leaf IS a panel**, and a single-card panel is a panel with one tab — so there is no
  "floating single card" special case, and no leaf↔tabs type transitions to get wrong.
- **`right_of(X)` is unambiguous**: a panel is the only region a split could address, because a card
  is never a region on its own. This was an open question only while the two types were separate.

### REGIONS COME FROM SPLITS

Stated loudly because it came up three separate times in design conversation: **you never need more
than one workspace to get multiple regions.** A left strip, a bottom strip and a main area is ONE
workspace with two splits. Arbitrary composition is what a tree of splits already gives you.

**At most ONE workspace per canvas** (and in M2, one workspace, full stop). Multiple workspaces
sharing a layer breaks on floating panels three ways — z-order cannot interleave between them,
tear-off has no answer for which workspace owns the new floating panel, and drag-to-dock would have
to move subtrees between separate data structures. One workspace makes all three vanish.

A fixed region that is not dockable at all is not a workspace either — it is free-authored content
sitting beside the workspace in the same container.

### Sizing lives on NODES, not on splits

Every workspace node carries an `axis_sizing` — the same `fixed()` / `grow_weighted()` vocabulary as
the layout engine:

- a fixed 280px strip → the panel has `fixed(280)`
- a 70/30 split → its two children have `grow_weighted(7)` and `grow_weighted(3)`
- dragging a splitter mutates the two children's sizings

An earlier draft put a `ratio` float on the split, reasoning that dragging a splitter changes the
relationship rather than either side. True for a PROPORTIONAL split — but a fixed-size region is
asymmetric (the strip is 280px, the other side takes what is left), which is a property of one child.
The layout engine already resolved this exact tension: containers do not carry ratios, children carry
sizing. Following it deletes "split ratio" as a concept and makes the workspace's sizing vocabulary
IDENTICAL to layout's, so emission is a direct translation rather than a conversion.

It also makes the collapse rule precise instead of hand-waved: since a split node has its own sizing
within its parent, **the surviving child inherits the split's sizing**. That was the vaguest step in
the invariants.

**`locked` is separate from sizing**, and the two are independently useful: `locked` means the
splitter is not draggable; sizing means how big the region is. A locked proportional region resizes
with the window but cannot be dragged; an unlocked fixed region can be dragged to a new pixel size
but does not scale with the window. Docking INTO a locked region still works — you can drop a card on
the party strip, you just cannot resize the strip.

### The workspace is a FOREST, not a tree

- one **docked tree** filling the container it is emitted into: splits whose leaves are panels
- N **floating panels**, each carrying a rect and a z

**This is what makes tear-off work.** Floating and docked are not two mechanisms: a floating panel is
the same object as a docked one, just rooted on its own. Every gesture is then a SUBTREE MOVE —
tear-off moves a subtree to a new floating panel, drag-to-dock moves one into the docked tree, undock
is the same move — so there is no floating↔docked conversion path to write, because there is no
conversion.

### The seam: card interiors are code, arrangement is data

**A card's interior is authored in code. Its placement is data the user edits.** The app NEVER states
where one card sits relative to another; it declares cards, and the workspace decides where they go.

Composition below the card line is **by plain function** — components are ordinary
`void f(Ui&, args)` functions with no registration, base class or framework. Composition above the
line is **by data**.

**DECISION — cards do NOT nest.** A card is the seam between the two composition mechanisms; nesting
one inside another puts a user-arrangeable atom inside a statically-authored tree, and every gesture
then has to answer "did I drop this into the workspace or into that card's interior?" — ambiguous
however it is resolved. It costs nothing: visual grouping inside a card is `u.element().themed()`,
which is a *presentational* grouping rather than an arrangement one.

### Why it is NOT a second layout tree (the constraint that shapes everything)

The user's objection to the textbook dock-tree architecture was correct: a dock tree is structurally
a layout tree with a restricted vocabulary — split ≈ row/column, panel ≈ container, splitter ≈ the
edge between children — so building one with its own rect-assignment walk re-implements layout in
miniature.

The resolution: **the workspace describes structure and computes NO geometry.** It walks and EMITS
into the `layout_builder` — a split becomes a row/column container, a panel becomes a tab bar plus
the selected card, and a card node calls that card's author. Every rect still comes from the one
layout engine.

A dock tree that computes rects duplicates layout. A dock tree that expands into layout does not.
**This is the invariant to defend in review: if `Workspace` ever grows a function returning a rect,
the design has drifted.**

The difference from the layout tree is not its shape, it is *who authors it*: the layout tree is
written by the programmer and rebuilt every frame; the workspace is edited by the user at runtime and
must survive that rebuild.

### Operations, and the seed

One operation set, used by gestures AND by the initial layout, so there is no second construction
path to maintain or test. Placements are free factories taking an `axis_sizing`, matching
`grow()`/`fit()`/`fixed()`:

```cpp
if (!ws.load(saved)) {
    ws.dock(PartyFrames, left(fixed(280)).locked());
    ws.dock(Chat,        bottom(fixed(200)).locked());
    ws.dock(Minimap,     right(grow_weighted(1)));
    ws.dock(Materials,   tab_with(Minimap));
}
ws.undock(Chat);   // becomes its own floating panel
ws.close(Chat);
```

The code-authored arrangement is a **seed**, not a static relationship: it runs only when there is
nothing saved, and the user owns the layout from that point on.

### Where the difficulty actually is

Insertion is easy. **Removal is where dock implementations get buggy.** With a leaf being a panel the
whole rule set is three steps and no type transitions:

1. remove the card from its panel
2. if the panel is now empty, remove the panel
3. if the panel had a parent split, replace the split with its surviving sibling, which inherits the
   split's sizing

A floating panel that empties is deleted outright. These invariants go in pure tree code with no
device, gtested like `update_panel`, and the gestures call exactly the same operations so there is one
implementation.

### Authoring API — DECIDED with the user (2026-08-01)

Everything is `u.X(...).config().content(closure)`, the same shape as every other factory in the
facade:

```cpp
u.workspace(debug_ws).content([&](Ui& u) {
    u.card(Passes).title("Passes").content([&](Ui& u) { pass_table(u, renderer); });
    u.card(Targets).title("Targets").content([&](Ui& u) { target_grid(u, renderer); });
});
```

Free-authored content sits alongside a workspace in the same container — this is how dockable HUD
pieces coexist with world-anchored ones:

```cpp
author_nameplates(u, scene, budget);          // world-anchored, never dockable
u.workspace(hud_ws).content([&](Ui& u) {
    u.card(Chat).title("Chat").content([&](Ui& u) { chat_log(u, log); });
});
```

**"Dockable" is therefore not a property of a canvas or of a card** — it is just whether a card is
currently referenced by a workspace. The same card can be docked in a debug workspace or floated on
the HUD, because a card knows nothing about placement. That is the seam doing its job.

**Why a card's interior cannot simply emit at its call site:** in a single-pass immediate builder,
emission order IS tree structure — there is no separate "position". The workspace decides structure,
so a card's body must run when the workspace says. Forced by immediate mode plus data-driven
placement, not a preference.

**Why a fluent card handle rather than `card(id, fn)`:** a card needs configuration beyond its body —
a title at minimum — and a tuple form puts two strings side by side, which is the identity-vs-content
ambiguity that got `label(name, text)` deleted. Rejected drafts also included a separate
declare-then-place step (adds an ordering rule: forget the second call and nothing draws) and an
`auto ws_view = u.workspace(ws)` handle (a variable that exists only to be a receiver, unlike
anything else in the facade). `u.card(...)` instead sits in the same family as `u.element()`,
`u.button()`, `u.panel()`.

Implementation note: `card()` needs `Ui` to know it is inside a workspace, which is the
`authoring_panel_` mechanism from M1b — a member set for the duration of the closure. Already proven.

**The alternative that was NOT taken**, recorded so it is a decision: cards could author immediately
into DETACHED SUBTREES which the workspace then splices into position. That is the only option where
bodies truly run where written (which matters for profiler zones, breakpoint stacks and mutation
order). Rejected for now because it needs real L0 machinery — detached roots, a splice that
re-indexes parent/child links, a merged text table, and one extra copy of every card's nodes. If
deferral bites in practice this is the escape hatch, and it deserves its own milestone rather than
being smuggled into M2.

### Naming conventions (match the existing code)

The codebase has a two-tier convention: **snake_case for plain data and free factories**
(`panel_rect`, `panel_state`, `axis_sizing`, `grow()`, `fit()`, `fixed()`), **PascalCase for fluent
handles and persistent owners** (`Ui`, `Element`, `Panel`, `Motion`, `Theme`, `PanelStore`).

So: `Workspace`, `Panel`, `Card` (PascalCase); `workspace_node`, `placement` (snake_case);
`left()`, `right_of()`, `below()`, `tab_with()` (free factories). Names like `WorkspaceAuthor`,
`PanelDecl` and `dock_target` were rejected as foreign to this codebase.

### Consequences for M1's work

`PanelStore`'s rect and z stop belonging to a CARD and start belonging to a floating PANEL. Not a
large change — `update_panel` is pure and takes a rect either way — but M1's per-panel rect becomes
per-floating-panel, and z becomes per-floating-panel rather than per-card. More correct anyway: a
floating group should raise as a UNIT, not per-tab.

### LATER (not M2): Canvas

A **canvas** is an ordered LAYER — draw/hit ordering plus an optional modal flag. It is orthogonal to
docking: it answers "in what order does this draw and receive input, and does it block what is
below?", which applies to ALL UI content, where a workspace answers "how are these dockable things
arranged?", which applies to a small subset. A canvas owns **0 or 1** workspaces, and the 0 case is
the common one (HUD, menus, tooltips have no dockable content at all).

**It is not new capability.** The codebase already has canvases hardcoded: `element.overlay` is a
1-bit canvas layer, and `kDebugZ = 250` in `debug_panels.cpp` is a third layer smuggled in as a z
convention. Canvas names and generalises them, and drops onto the M1b batching key with no new
mechanism: `(overlay << 8) | z` becomes `(canvas << 8) | z`, with `hit_test_layered` still comparing
one key.

**Deliberately NOT built in M2.** Docking does not depend on it — M2 works against the existing
overlay/z exactly as M1 does. What canvas buys (a modal main menu, an ordered layer stack) is a
game-client concern, not a docking one, so building it now would add a concept before the thing that
needs it exists. It lands when there is a menu or a second layer to order, and by then we will know
whether modality wants to be a flag or something richer.

**Interim convention to write down**: content that must draw above floating panels (tooltips, drag
previews) uses a high `z`, as `debug_panels.cpp` already does. There are two ad-hoc users already; a
third would make it a pattern nobody decided on.

### Deferred: identity scoping

Reusable components with internal named parts (a slider's track and thumb) will collide on ids when
instantiated twice. The fix is NOT a new API: **make the existing tree nesting carry identity** — a
named container qualifies its named descendants, an id-less container passes the scope through
(consistent with id-lessness already meaning "decoration"). This is standard for immediate-mode UI
(ImGui's ID stack, egui's parent-derived ids, Clay's local ids), and deriving from the named ancestor
avoids the push/pop pair that can get out of balance.

**Not built yet: nothing collides today.** It becomes load-bearing in **brief 13**, when the widget
kit introduces components instantiated more than once — which is also when we learn whether the
derivation rule is right from real widgets rather than a sketch. `Panel` currently hand-rolls it
(`name + ".title"`, `name + ".grip"`); that is the one call site it would simplify today.

### Overflow policy — DEFERRED, with the reasoning (user, 2026-08-01)

A panel can be resized smaller than its contents. Text does not spill (glyphs clip to their own node
box in `ui_pass.cpp`), so the failure is visible and local — a too-small panel truncates its own
text — but nothing stops it happening.

**The engine floor (`panel_limits`) is a plain constant that knows nothing about content, and stays
that way.** The alternative considered and REJECTED was deriving the floor from the measured content
extent. That needs a post-layout writeback into `PanelStore`, which costs:

- **a new frame phase** (author → layout → measure → store). `Ui::end_frame()` runs before
  `builder.end(available)`, so there is nowhere to hang it today. New ordering constraints are the
  bug class that produced the froxel device-lost crash and the `ensure_hiz` VUID — a writeback is
  correct only if it runs at the right point and silently wrong if reordered.
- **a muddied store.** `PanelStore` currently means "where the user dragged it" — pure intent. A
  content extent is derived data, and mixing the two matters if persistence ever lands (you'd be
  serialising derived values alongside intent).

And it is INTERIM work regardless: the floor is a proxy for the real missing feature, an overflow
policy. **Once text wrap and scrolling exist, "smaller than its contents" stops being a defect and
becomes the case scrolling is for**, and a content-aware floor is deleted.

So: **authors set honest `limits()`** on panels whose content has a real minimum — the author is who
knows what is in it. The usual objection (future authors guess badly) is defanged by the clipping: a
bad guess is self-evident on sight, not a silent corruption.

**The real fix, when scoped:** word-wrap already exists for fixed-width text nodes
(`ui_pass.cpp:154`); the gap is that `fit` nodes render as one unwrapped line. Scrolling is a genuine
feature (clip rect + offset + scrollbar widget) and belongs with **brief 13's widget kit**, not
smuggled into the panel container.

### Explicitly OUT of scope (decided, do not drift into)

- **Promoting `UIPass`** (the rings/SDF/atlas-upload draw pass). It is generic machinery wearing a
  sandbox namespace and is the natural end state of option (3) — but moving it means moving shader
  ownership. Named here so it is a *decision*, not an ambiguity.
- **Promoting `DebugConsole`.** A real candidate (pure logic fronting the engine CVar registry), but
  its command set is sandbox-flavoured (in-engine PNG compare) and splitting generic-core from
  sandbox-commands is a distraction from this brief.

## Verification (per `README.md` rules)

- **Layout-tree dump diff (M0a) is the primary mechanical gate** for L0–L4: brief-05 screens must
  produce a byte-identical tree dump before and after the facade migration. Baseline captured in
  M0a, re-checked every milestone through M0c. Visual verify is the gate only for what the dump
  cannot see (motion, docking feel, theme).
- **Engine UI layers L1–L5 unit-testable without a device** — `ui::interaction` (value type),
  `Motion`, theme resolution, sizing, dock-tree operations and layout (de)serialisation all get
  gtest coverage. Loose coupling is a verification requirement here, not just a style preference.
- Existing brief-05 mock screens (`STRING_SCENE=ui`, all `STRING_UI_SCREEN` variants) behave
  identically through the new facade.
- 500-nameplate stress holds the brief-05 numbers (watch immediate-mode CPU cost — brief 13 owns
  the reduction).
- Docking: drag/dock/split/resize solid at multiple resolutions incl. non-square; layout persists
  across restart; corrupt/old layout file degrades gracefully.
- Gamepad focus nav + motion still work through the facade.
- **NEEDS VISUAL VERIFY** (docking feel, drag-to-dock, resize, theme) — agent reports the list.
