# Brief 14 — Debug UI: inspection

Status: **DRAFT — seeded 2026-08-01 from a design conversation while finishing brief 13.** Only the
parts actually decided are written down; the rest of the brief is still to be scoped. Part of the
tooling/UI arc (11–15) before 08 VFX.

Deps: brief 11 (pass/target introspection — `Renderer::passes()`, `Renderer::resources()`,
`gpu_timing().stats()`), brief 12 (panels/workspace), brief 13 (widget kit).

## Scope reminder

Render/pipeline debug and bounded lookdev inspection — NOT a scene editor. Manipulation (light
gizmos, live material edit) is brief 15.

## The two target-inspection tools — DECIDED with the user (2026-08-01)

They cover **disjoint sets of targets**, which is why both exist rather than one replacing the other.

### 1. The lens (magnifying glass)

A draggable circular region over the rendered frame. Inside it, the frame is replaced by a chosen
debug view, at the **same screen pixels**. Inspired by Decima's debug loupe.

**Its whole value is spatial correspondence** — you point at an artifact and ask "what is this pixel's
normal / roughness / overdraw, *here*". That is exactly what a panel thumbnail throws away.

**TWO INDEPENDENT AXES**, which is what makes it more than a viewer:

| axis | range |
| --- | --- |
| WHAT | the scene itself, or a debug target substituted in place |
| HOW MUCH | 1x (exact correspondence) up to Nx (pixel loupe) |

Magnification defaults to **1x**, so correspondence is the default and zoom is opt-in. At Nx on the
SCENE with no substitution it is a plain pixel loupe — the tool you want for AA and single-pixel
artifacts. Both fall out of one shader.

**WHERE IT LIVES: the composite/post pass, NOT the UI pass.** It is a full-screen substitution, and
those passes already sample the scene with bindless `Sampler2D` arrays, already declare their target
reads, and already write the output. The UI's role shrinks to **chrome and controls** — the lens rect
as draggable state, an outline, a combo for the target and sliders for range and magnification, all
of which brief 13's widget kit already provides. No new UI rendering machinery at all.

**Sampling:**

```
src_uv = lens_centre_uv + (pixel - lens_centre) / (screen_size * magnification)
```

At magnification 1 this reduces to identity, so exact correspondence is structural rather than tuned.

**MAGNIFIED SAMPLING MUST SNAP TO TEXEL CENTRES.** A linear sampler at 4x produces a blurry smear,
destroying precisely what is being inspected — AA artifacts and fireflies ARE texel-level detail.
`(floor(uv * size) + 0.5) / size` makes the existing linear sampler behave as nearest; no second
sampler binding is needed.

**CORRECTNESS TRAP — debug targets must BYPASS TONEMAPPING.** Applied in composite, lens pixels
showing raw data (normals, roughness) would otherwise get exposure and tonemap applied and come out
wrong. Same discipline as the UI cancelling exposure to stay display-referred. Easy to get wrong and
hard to spot, because a tonemapped normal buffer still looks like a plausible image.

### 2. The image widget / target browser

A target drawn into a **panel**, with channel mask, mip selection, range remap and false colour.
Carried over from brief 13 M2, which deferred it here.

**THE LENS CANNOT REPLACE THIS.** In-place substitution is only meaningful for SCREEN-SPACE targets.
A **shadow map is in light space**; a cubemap face, an IBL atlas and a probe atlas have no screen
position to substitute at. Those can only be inspected in a panel. Depth prepass output happens to be
screen-space so either tool serves it, but shadow maps are image-widget-only.

It is also the "which pass produced garbage" tool — many targets at once, for orienting when you do
not yet know where the problem is. The lens is the "what is this pixel" tool once you do.

**What it needs** (from a read of the code, 2026-08-01): the sampling path already exists — the text
shader does `textures[pc.atlas_slot].Sample(...)` against bindless `Sampler2D textures[]`, with slots
from `descriptor_table_.get_binding_slot(image, TEXTURE)`. The work is:

- `element.image` as a 1-based index into a builder-side table, mirroring how `text` is held out of
  line so `element` stays small and trivially copyable.
- A third stream in `ui_pass`: pack + pipeline + a draw inside each layer batch. Brief 12 M1b's
  per-layer batching already accommodates a third draw.
- A shader: channel swizzle, `SampleLevel` for mips, range remap, false-colour ramp.
- The widget API.

**THE RISKY HALF IS NOT THE WIDGET — it is sampling a render target from the UI pass**, which needs a
dynamically declared `SampledRead` on whatever target is currently selected. That is expressible
(brief 16 made `PassSpec.usages()` a live getter read each frame), but wrong layout / missing barrier
/ target-absent-this-frame are all real failure modes.

**DE-RISKING: the glyph atlas is already a bindless sampled texture with a known slot.** The whole
widget, shader and pipeline can be built and verified against it with zero barrier risk, before any
render target is involved. Channel masking, mip selection and range remap are all visible on an SDF
atlas.

## The graph views — DECIDED with the user (2026-08-01)

Three candidates were pitched. All are derivable from data the planner already computes: `GraphPlan`
holds `passes` (with per-pass `ResourceUsage`s), `adjacency` (the real pass→pass edges),
`toposorted`, and `resource_lifetimes`.

**CHOSEN: the lifetime timeline first, then the pass DAG. The bipartite resource graph is REJECTED as
a view and folded into the DAG via selection.**

### Rejected: bipartite pass+resource graph

Nodes for both passes and resources, edges for reads/writes. The question it answers — "who wrote this
before I read it" — is the right one, but it does not earn a second node type: ~15 passes each
touching several resources makes the layered layout dense, and Manhattan routing through that many
crossings is where the graph widget looks worst. **Its value is recovered by SELECTION in the DAG
below**, which costs nothing extra because the widget already highlights a selected node's edges.

### C (first) — resource lifetime timeline

Not a graph. X is toposorted pass position, one row per resource, a bar spanning `first`..`last` with
`first_writer` marked. Rows and boxes; no graph widget.

**Why first:** it answers a question nothing else does — which resources are alive when, and which
spans overlap — and **brief 16 built the transient arena but explicitly DEFERRED real interval
packing. This is the instrument for that deferred work.** It would also have made the `hz.depth`
cross-frame resting-layout problem legible as a span that does not close inside the frame.

### A (second) — pass DAG, with provenance by selection

Nodes are passes, edges come straight from `plan.adjacency`; colour by kind, dim the toggled-off, GPU
time per node from `gpu_timing().stats()`. **`adjacency` is the same array the barrier derivation runs
on, so this is ground truth rather than a redrawing of anyone's mental model** — when a barrier shows
up somewhere surprising, this is where the reason lives. Select a pass → its resources list beside it;
select a resource → its producers and consumers light up. `.requires()` transitive-skip chains are in
the same data, so "what else dies if I toggle this off" is answerable.

### Honest limit on both

**The defects that actually cost this project most were invisible to the graph** — the froxel
update-order device-lost and the `ensure_hiz` descriptor invalidation were not graph edges. These
views are for UNDERSTANDING the frame, not for catching that class. Do not let them accrete scope on
the promise of catching sync bugs.

## Milestones

- **M0 — almost nothing, after a false start worth recording.**

  The first draft of this milestone added DEBUG NAMES for resources: `resource_id` is a raw integer,
  neither `ResourceRegistry` nor `resource_allocator` stores a string, so a timeline row would be
  labelled `4294967297`. The plan was an optional name at ~26 declaration sites plus a name table on
  the registry. **The user pushed back — "a lot of plumbing that kinda creates spaghetti" — and was
  right: that is new string-carrying surface threaded through a system that otherwise has no opinion
  about strings, to label rows in a debug panel.**

  **DERIVE THE LABEL INSTEAD, from data that already exists.** A row is identified by the pass that
  first writes it: `ResourceLifetime::first_writer` and `CompiledPass::name` are both already there.
  A row reads `geometry.phase1 ▸ 0`; multiple writes by one pass disambiguate by stable index within
  its usages; the three target sentinels get names from a three-line switch in the DEBUG layer, not
  the engine; a resource with no writer is an external input and says so. **For a lifetime view this
  is arguably the better label anyway — the chart is about production and consumption, so naming a row
  by its producer is the honest identity rather than a substitute for one.** If it proves ambiguous in
  use, add real names THEN, with evidence about which resources were actually confusable.

  The remaining item is not a prerequisite either, and the answer got simpler still on a second read:
  **`GpuProfiler` already solves this exact problem with a process-global read-only handle**, and its
  comment says why — "so a UI/tooling layer (the profiler HUD) can read the numbers without the
  renderer being plumbed through the UI author". `debug_panels.cpp` already consumes it that way.
  **Mirror it: a `GraphIntrospect` snapshot the renderer owns, registers globally on construction and
  clears on destruction.** A global is a blunt instrument, but it is the established local convention
  for read-only debug data, and matching the neighbour beats inventing a second mechanism beside it.
  Zero changes to `demo_scene`, `UIPass`, `UiContext` or `DebugPanels`' signature. Do NOT give the UI
  pass a `Renderer&`.

  One real trap to respect: `resource_lifetimes` positions index the TOPOSORTED/compiled order, while
  `Renderer::passes()` lists every AUTHORED pass including disabled ones. The X axis must be the
  compiled order or every bar points at the wrong pass.
- **M1 — Menu bar (DECIDED with the user 2026-08-01, placed ahead of the views because it is the
  surface they are reached from).** A File/Edit/View-style bar with dropdowns: the home for global
  toggles that do not deserve panel real estate (`r.*` cvars, view modes, opening the debug panels)
  and, later, `View ▸ New lens`.

  **`Combo` deliberately expands IN FLOW rather than as a floating popup, and its header says why: a
  popup must be positioned at its button's screen rect, "which does not exist until after layout".
  THAT BLOCKER IS GONE — `Ui::observed_box()` (added for brief 13's scroll area) is exactly that
  post-layout anchor.** The rest is already there: `.floating()` + `.overlay()` + `.z()` to draw
  above everything, and `hit_test_layered` resolves overlay-then-z so the popup wins the click.

  It is a NEW WIDGET, not a `Combo` variant, because the interaction differs: hover-to-switch once a
  menu is open, click-outside-to-close, later submenus. It is chrome, not a panel (brief 12's model:
  composed elements, like the HUD and tooltips). Code lands in the widget kit per brief 13's line —
  interaction, not meaning — even though this brief is what motivates it.
- **M2 — C: the lifetime timeline panel.**
- **M3 — A: expose `adjacency` + per-pass usages through the snapshot; the pass DAG with
  selection-based provenance.**
- **M4 — the image widget / target browser** (see above; de-risk against the glyph atlas first).
- **M5 — the lens** (see above; lives in composite/post).

  **N LENSES, NOT ONE — DECIDED with the user 2026-08-01.** The prose above says "the lens",
  singular; the useful feature is plural. Two lenses put normals here and roughness there at the same
  instant, or one at 1x for correspondence beside one at 8x for pixel detail — workflows a single lens
  cannot do at all.

  **Decide the DATA LAYOUT for N now and ship the UI with one.** The shader cost is a bounded loop
  over a `lens_count` uniform instead of one branch (cap 4); the uniform/push-constant layout is the
  part that is expensive to retrofit, so it is the part that must not be provisional.

  Creation and manipulation reuse what exists:
  - **`View ▸ New lens`** in the M1 menu bar, created at screen centre.
  - **Drag and resize come free from `update_panel`** — a pure function over a `panel_rect` plus
    interaction, handles and limits. A lens IS a movable, resizable rect, so it inherits the
    origin-capture anti-drift discipline that took several corrections to get right on panels. No new
    drag code.
  - **Controls in a docked card, one row per lens** (target combo, magnification slider, close), NOT
    floating over the viewport. On-frame chrome stays an outline and a grip — the entire point is
    seeing the pixels underneath.

  Note the direction of flow: lens state goes **UI -> pass**, the opposite of the graph snapshot's
  pass -> UI. Both have precedent (cvars already go that way), but do not assume the read-only global
  handle is bidirectional — it is not.

## Still to scope

Per-pass toggle + timing rows (small: `PassInfo` has name/kind/flags/enabled, `gpu_timing().stats()`
pairs by name, and `r.pass.<name>` cvars already toggle), material preview sphere (needs a mini-pass
to render it AND the image widget to show it — the least-known piece). Not yet designed — do not treat
this section as decided.

## M3 — done 2026-08-02

Snapshot gained `Pass{name, uses, successors, depth}` (replacing the bare `pass_order`), plus
`dropped` — authored passes that never reached the compiled plan. `adjacency` is remapped out of
PLANNER index space into COMPILED (toposorted) space in `refresh_introspection`; getting that
inversion wrong would draw a plausible-looking graph wired to the wrong passes, so it is the one
line in here worth re-reading. `Use.resource` indices are filled AFTER the resource vector is
sorted, for the same class of reason.

**It is a real node canvas with drawn edges.** The first draft was rows of chips with no edges, on
the stated reasoning that the layout engine has no absolute child placement. That was wrong — I
reasoned from the M2 timeline's spacer trick without checking. `float_local` exists and its comment
names this exact case ("a graph canvas laying out nodes, for instance"): `.floating(x, y).local()`
places children in the parent's own coordinates. The panel computes node positions itself and
Manhattan-routes each edge as three thin rects (drop / run / rise) through the channel below its
source row, authored BEFORE the nodes so a long edge slides behind any row it passes.

User verdict on the first draft: "it just doesn't really read as a graph." Correct, and the fix was
not a workaround — the facility was there and purpose-built.

**Rows are longest-path depth, not execution order.** Passes sharing a row are genuinely
independent — neither can reach the other. The toposort picks one arbitrary valid sequence and hides
that; depth shows it.

**DebugPanels owns a SECOND `Ui`, and it was never observed.** `ScrollArea` sizes its extent from
`Ui::observed_box()` — last frame's laid-out content height, the one number the author cannot supply.
`Ui::observe()` was only ever called on the app's shared `Ui` via the UI pass's post-layout hook; the
debug shell's own `Ui` had nobody calling it, so `observed_box` returned an empty box, `content_h`
read 0, `max_off` was 0, and scrolling was inert while clipping still worked — which is precisely the
symptom ("it no longer compresses, but the scroll amount doesn't scale to the contents"). Fix:
`DebugPanels::observe()`, and the panels object is now a `shared_ptr` held by BOTH the author closure
and the post-layout hook — the same seam, for the same reason, as `fluent` and `Workspace`.

Measurable tell: the scrollbar thumb was `8x364` (full track = "nothing to scroll") and is now
`8x321` = 364 x (364/412), proportional to real measured content.

GENERAL RULE this exposes: any object that owns its own `Ui` must be observed, or every
self-measuring widget inside it silently degrades to "empty" instead of failing loudly.

**Two scroll viewports, not one.** The graph and the resource list each get their own extent
(`dag_canvas` ~60%% of the body, `dag_list` ~40%%). Sharing one viewport meant scrolling the list
scrolled the selected pass off the top, which defeats the point — the list only means anything
next to the node it describes. Selecting a pass appends one row per resource it touches,
and `geometry` touches enough to run off the bottom — which is what happened the first time it was
clicked. A panel whose content length depends on selection cannot assume it fits, so it does not:
the body is a fixed-height viewport and the content is clipped and scrolled inside it. Capping the
canvas to that viewport also exposed an off-by-one-gap — `widest * pitch_x` counts a TRAILING gap,
which pushed the last node of the widest row a few px past the edge and clipped it. The canvas is
nodes plus the gaps BETWEEN them.

Navigation: click a pass → predecessors green, successors amber, its R/W resource list below; click
a resource row → pivots selection onto that resource and lights up its producers and consumers.
Panel `dbg.dag` / F6 / `View ▸ Pass DAG`, default off. GPU ms per node comes from
`GpuProfiler::global()` matched by name.

### What it found on its first run — FIXED 2026-08-02

In the lookdev scene the DAG puts `hiz.build` at **depth 0**, i.e. with no predecessors, while
`geometry` is at depth 1. That is not a display bug — it is the graph. The introspection log
confirms it: the image `hiz.build` samples has `first_writer=-1` (nobody writes it in the graph),
because `hz.depth` is produced by the render-pass MIN-resolve, which the graph cannot observe — the
tracker's `seed()` supplies that state from outside. Its own pyramid write IS an edge
(`first_writer=6`, consumed by `geometry.phase2` at 7), so only the incoming dependency is missing.

Consequence: `geometry → hiz.build` ordering is currently held by authoring order and the toposort's
tie-break, not by a declared dependency. It is correct today and byte-parity holds, but nothing in
the plan would stop a future reordering of "independent" passes from scheduling `hiz.build` before
the depth it samples. Fixing it means declaring the resolve write in the graph — a sync change that
needs its own gating run, so it is recorded here rather than done as a drive-by.

This is precisely the use the brief predicted: "when a barrier shows up somewhere surprising, this
is where the reason lives."

### The hiz.build fix

`geometry.phase1` now DECLARES that it produces `hz.depth`, via `PassSpec::use()` in the author
block with the logical image handle.

The key structural fact that made this small: **the plan's usages and the executor's live usages are
already separate paths.** Barrier derivation reads `cp.exec.usages()` (the getter bound to the pass's
live member); adjacency, the toposort, lifetimes and the M3 introspection read the plan's declared
lists. `use()` appends to the declared lists ONLY. So the declaration buys the edge, the ordering
constraint and the lifetime span, and changes no barrier and no resolve behaviour.

Sync was never wrong — the renderer seeds the tracker with the post-resolve state and `hiz.build`'s
SampledRead derives its barrier from that. What was missing was that the PLANNER could not see the
dependency, so the order rested on authoring order and the toposort's tie-break.

Deliberately NOT done: making `is_write(Access::DepthResolve)` true. That marker is appended at
record time (after compile), so the planner would never see it anyway — and flipping it would change
how the group loop classifies the live usage, i.e. barriers. The declaration is separate from the
marker on purpose, and both are commented to say so.

Verified: `hz.depth` went from `passes [6..6] first_writer=-1` to `passes [5..6] first_writer=5`
(geometry), and `hiz.build` moved from depth 0 to depth 2 — directly below the pass that feeds it.
Gated across the full battery: exterior, non-square 1600x900, and fixed-dt orbit (`STRING_ORBIT=1`)
all AE=0, validation clean, checks default/ui/cook green, 5/5 UI baselines.

## M4 — done 2026-08-02 (de-risked half)

`element.image` is a 1-based index into a builder-side `image_run` table, mirroring `text` exactly.
`image_run::source` is an OPAQUE id (`image_source::glyph_atlas` = 1), NOT a bindless slot — the UI
kit depends on nothing and must not learn what a texture is. The renderer owns the mapping.

Third draw stream in `ui_pass`: `GpuImage` ring, `image_shader.slang`, `DrawBatch` gains
`image_first/count`, drawn **shapes -> images -> text** per batch so an image sits on its panel
background and under any label over it. The pack branch is `image != 0` FIRST, so every existing node
takes exactly the branch it took before — additive, not a reordering, which is why byte parity held.

Shader controls: channel mask, `SampleLevel` for an explicit mip, range remap, and a viridis-ish
false-colour ramp (perceptually monotonic — a rainbow reverses lightness partway and invents edges
that are not in the data). A single masked-in channel renders GREY, not tinted: isolating a channel
asks "what is in it", and a colour cast makes values harder to compare between channels.

Panel `dbg.image` / F7 / `View ▸ Image browser`, default off.

### Verified against the glyph atlas, per the brief

Headless capture of the 250x250 viewport: stddev 0.20, per-channel R=G=B=0.106 — real structure,
correctly greyscale. This proves the whole path (side table -> pack -> ring -> pipeline -> bindless
sample) with zero barrier risk, exactly as the de-risking called for.

**A default the capture caught:** with the RGBA mask on a single-channel R8 atlas the sampler reports
the absent green/blue as 0, so it rendered RED (R=0.102, G/B=0.01). That reads as "the data is in the
red channel" rather than "this texture has one channel". The panel now defaults to the R mask, which
the shader draws as grey; stddev went 0.075 -> 0.20, i.e. far more visible structure.

### Still open — the risky half

Render targets. That needs a dynamically declared `SampledRead` on whatever target is selected
(expressible: `PassSpec.usages()` is a live getter), plus handling wrong layout, missing barrier and
target-absent-this-frame. None of that is touched here, and the widget is ready for it.

## M5 — the lens, done 2026-08-02 (with a verification limit worth knowing)

`LensState` (string-core, `vulkan/lens.hpp`) holds N lenses; `kMaxLenses = 4`, and the cap is
load-bearing because the shader cost is a bounded loop. Direction of flow is **UI -> pass**, so it is
a separate explicitly-MUTABLE store rather than a second use of the read-only global handle — the
brief's warning about that channel not being bidirectional was correct.

The rect IS a `panel_state` driven by `update_panel`, so move/resize/clamp and the origin-capture
anti-drift discipline come free — no second drag implementation to keep in step. Chrome is an
outline, a thin move strip and a grip; controls are a DOCKED card, one row per lens. `View ▸ New
lens` creates at 1x (correspondence default, zoom opt-in). Packed 16B per lens into the composite's
existing push block (88->96 bytes, inside the 128-byte guaranteed minimum).

Two traps the brief named, both handled: magnified sampling snaps to texel centres
(`(floor(uv*size)+0.5)/size`, making the existing linear sampler behave as nearest), and a lens
sourcing a debug target BYPASSES tonemapping.

A third the brief did not name: **at 1x on the scene the lens is a structural NO-OP** (`lens_uv`
returns false), not merely an algebraic identity. `centre/screen + (px-centre)/(screen*mag)` reduces
to `px/screen` on paper but not in floating point, and even returning the untouched uv still put the
sample inside divergent control flow. Only skipping the pixel entirely makes 1x bit-exact.

### VERIFICATION LIMIT — the headless capture cannot see the lens

`Renderer`'s debug capture copies `color_attachment_` (the resolved HDR target) and applies
`CompositePass::encode_display` **on the CPU**. It never runs the composite fragment shader, so no
capture-based gate can observe a composite-stage effect. The lens chrome and controls DO appear in
captures because those are UI drawn into the HDR target before composite — which is precisely what
makes this trap dangerous: a capture shows *something* changing in the lens rect and reads as proof.

It is not proof. Verified instead by probe: the reflected push range is 96 bytes and matches
`sizeof(push)` exactly, and `LensState` reaches the composite with `count=1` at the same address the
UI wrote (so no duplicate-instance or ordering problem). The image itself needs a LIVE check.

`dbg.lens_test=<magnification>` seeds one deterministic lens at a fixed rect — for live inspection,
NOT a headless gate, for the reason above.

Making this gateable means capturing the SWAPCHAIN rather than the HDR target. That is a real change
to the capture path and is not attempted here.

### Two defects found in live use (2026-08-02)

**Zoom made the lens ungrabbable.** The UI draws into the HDR target BEFORE the composite, and the
composite REPLACES every pixel of the lens rect with magnified content — so chrome drawn on top of
the rect was sampled away the moment magnification left 1. Fixed by moving all chrome into the ring
OUTSIDE the rect: outline at rect-2 inflated by 4, move strip above, grip past the bottom-right
corner. Better regardless — nothing now overlaps the pixels being inspected, which is the point.
Verified from a layout dump (chrome is UI, so a dump CAN see it, unlike the substitution itself):
frame 298,198 304x244 · move 298,186 304x12 · grip 602,442 14x14, all clear of the 300..600 x
200..440 rect.

A follow-up nit from the same live pass: the strip was sized to the RECT while the outline was sized
to rect+4, so the border overhung the bar by 2px each side and the two read as separate pieces of
chrome. The strip now spans the outline's width — same left edge, same width — so they stack as one
frame.

**Bypass tonemap showed flat white outside Sponza.** It was returning the raw sample with NO
exposure. The scene target is scene-referred kilo-units, so raw values are enormous and `saturate()`
clamped everything bright to white. Bypass means skip the TONEMAP CURVE, not skip exposure —
exposure is what brings a scene-referred buffer into the display range; the tonemap is what reshapes
it. Now returns `saturate(hdr)` (exposure applied, LUT skipped): a linear un-tonemapped view where
highlights still clip, because that is what un-tonemapped means, but everything below clipping is
readable.

The second one is a good illustration of the verification limit above: neither defect was reachable
by any capture-based gate, and both were found by looking at the running thing.

### The clamp rule changed: headers, not panels (2026-08-02)

Third from the same live pass, and the one that turned out to be a real rule rather than a nit: the
lens's move strip could slide off a screen edge. Its chrome lives OUTSIDE the rect (see above), so
clamping the RECT into the viewport — which is all `update_panel` ever did — still let the strip go
over the top edge, and the lens was then unreachable.

`panel_limits` now carries a `panel_header` describing where the grabbable strip sits, as offsets
from the rect (`dx/dy/dwidth` + an absolute `height`), and the clamp keeps THAT rect wholly on
screen. Offsets rather than an absolute rect because the two cases differ only in these numbers: an
ordinary panel's title bar is the top strip OF the rect (all zero, height filled in from the theme's
`font_px_title + pad` by `Panel::content`, so no author has to restate it), while the lens's strip
floats above the outline (`{-2, -14, +4, 12}`).

This REPLACES `keep_visible`. That rule — keep any 48px of the panel on screen — was both too weak
and the wrong axis: it permitted exactly the half-eaten header the user hit, while the thing it was
protecting (never strand a panel) is better served by protecting the handle directly. The body is
now free to hang off any edge, which is what the user asked for and what a panel taller than the
viewport needs anyway. Degenerate case: a header wider than the viewport inverts the bounds, and is
clamped to the inverted range so it SPANS the screen instead of being flung to one side.

The resize clamp bounds width by the header's right edge for the same reason — widening the panel
widens the header with it.

Verified: 5 unit tests (including the outside-the-rect and wider-than-viewport cases), all 5 UI
baselines byte-identical (the client screens sit well inside the viewport, so a correct rule change
is invisible there), exterior AE=0, and a 400x300 dump with a seeded lens showing the rect pulled
from x=300 to x=98 so the 304px strip lands flush on the right edge, while the frame's bottom stays
off-screen at y=442 — header in, content out, exactly the split asked for.

Known and left: the resize GRIP can still leave the screen with the body. It is recoverable (move
the panel back with the header, which cannot leave), so it does not have the stranding property that
made the header case a bug.
