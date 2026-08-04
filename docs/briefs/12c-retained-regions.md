# Brief 12c — Retained regions: sinking the per-frame authoring cost

Status: **DRAFT / for discussion** (written 2026-08-03, after 12b closed and measurement showed
authoring is now ~96% of the UI frame at stress scale). This is brief 12's deferred **M4
retained-on-immediate**, finally scoped. Deps: 12b M0–M3 (all landed) are the load-bearing
prerequisites — persistent identity (M1 surfaces), retained product storage (M3 box columns),
change-detection discipline animation cannot defeat (M3 input classes), and position decoupled from
content (M2). Nothing here re-decides brief 12's API conventions.

## Goal

Make an unchanged UI cost **nothing**: no closure, no node emission, no layout, no buffer repack,
no upload. Not "cheaper" — nothing. The target is energy, not framerate: a static debug shell over a
moving 3D scene should be indistinguishable from not having a UI at all.

## Why now — measured, not assumed (2026-08-03)

12b removed everything downstream of authoring. What that exposed:

| stage | when it runs (today) | share of UI CPU (M0, real demo) |
|-------|----------------------|----------------------------------|
| authoring — closures, node emission, `own()`, signature folding | **every frame, always** | **48–56%** |
| layout — fit/flex/wrap/position | dirty surfaces only (12b M3) | 9–16% |
| shaping | cache miss only (12b M0) | (inside layout) |
| GPU buffer repack | **every frame, always** | **33–36%** |
| upload (memcpy to mapped rings) | every frame, always | 0.4–0.5% |

(The repack row was `NOT MEASURED` when this brief was drafted; M0 filled it in, and the answer
changed the scope — see M0 below.)

Authoring is **not amortized and never was**: `ui_pass.cpp` calls `builder_.clear()` and re-runs the
author closure every frame. That is not an implementation detail, it is what immediate mode IS —
`if (visible) u.panel(...)` works precisely because the tree is rebuilt from nothing.

The authoring ladder (2000 nameplates, author phase only) says where it goes:

```
own(concat + to_string)    2236us
prebuilt name              1564us   (-672us: building the string)
literal name               1552us   (-13us:  the arena copy is ~free)
plain child, no text       1086us   (-465us: own() + the text table)
raw builder, no facade     1047us   (-40us:  the facade is ~2%)
```

So the facade is not the problem and the arena is not the problem. ~51% is app-side (rebuilding a
unique per-entity string every frame) and ~47% is emitting the nodes at all. **Only not running the
closure removes the 47%.** That is this brief.

Scale note, so this is not scarier than it is: the realistic debug shell is 124 nodes / 62us of
authoring — irrelevant. This matters at HUD-with-thousands-of-world-anchored-elements scale, and for
ENERGY at any scale, because "irrelevant" still means waking the CPU every frame forever.

## The decision that shapes everything (settled with the user, 2026-08-03)

To skip authoring, the closure must not run — and **running the closure is currently how we detect
change**. So something must assert "nothing this subtree reads has changed" without executing it.
Three models were considered; the user chose **auto-tracked handle reads**.

- **Auto-tracked (CHOSEN)** — a region records what it READ while expanding; next frame those reads
  are re-checked, and if none changed the closure is skipped. No deps list to maintain and no
  stale-UI bug class from forgetting one. **Dirt is derived, not declared** — the same principle
  that retired the render graph's hand-rolled barriers, applied to UI.
- **Explicit memo keys** — rejected: much smaller, but correctness moves to the author and a missed
  dependency is a silently stale panel. That is the React deps-array bug, and it is miserable to
  diagnose precisely because nothing detects it.
- **Full retained tree** (author once, mutate via handles, never re-expand) — rejected: it sinks the
  cost furthest but breaks "immediate mode stays the core" (locked, brief 12) and turns `if` and
  `for` into explicit tree mutations.

### Why the locked `bind()` rule is what makes this possible

`ui.hpp` already ships two binds, and the comments already say why:

- `bind(handle)` — "the RETAINED-SAFE canonical path. The handle outlives the frame, so a retained
  tree may keep this binding."
- `bind(obj, &Obj::member)` — "the IMMEDIATE-ONLY escape hatch... a retained tree must NOT hold one."

A region checks its deps by **keeping its handle-backed bindings alive and re-reading them**. That
is legal for exactly the first form and illegal for exactly the second — the rule was written for
this and now has to be enforced rather than merely documented.

**Deps are compared BY VALUE, not by a version counter.** This needs no change to the
`accessor_handle` concept (`h.get()` / `h.set()`), so `CVar<T>` and brief 15's material/light store
both work unmodified. Re-reading a handful of handles is orders cheaper than re-authoring a subtree,
and value comparison cannot go stale the way a hand-bumped version can.

### The honest hole, and the net that covers it

Auto-tracking only sees reads that go THROUGH the tracked API. A closure that reads a captured
`float` directly is invisible, and its region would go stale. Convention ("retained regions read
only handles") is not enforcement.

So the brief ships the net with the feature: **`STRING_UI_VERIFY_RETAINED=1` re-authors every region
every frame and asserts the emitted nodes match what was retained.** An untracked read becomes a
loud, located failure in a debug run instead of a panel that quietly stopped updating. This is the
same shape as 12b's `STRING_UI_NO_SKIP` and for the same reason: a cache nobody can audit is a cache
nobody can debug.

## The model

**A named surface is a retained region. There is no keyword.** Retention is inferred by the
framework, exactly as 12b M3's clean-surface layout skip is — no author anywhere types anything to
get that, and this is the same mechanism one level up.

```cpp
// This is what you already write. It retains.
u.panel("stats").title("Stats").content([&](Ui& u) {
    u.text(stats.name());       // handle read -> tracked
    u.bar(bind(stats.health));  // handle-backed bind -> tracked
});
```

The API delta for the common case is **zero**. Settled with the user 2026-08-04, replacing an earlier
sketch of `u.retained(name, body)` — which was rejected for three reasons worth recording, since they
generalise: it introduced a SECOND identity for a subtree that already had one (the panel's name); it
nested a closure inside a closure that already took a body; and its `[&](Ui& c)` parameter *looked*
like a scope without being one — capturing `u` and writing `u.` inside would author outside the
region while reading as if inside it. Underneath all three: it put a cache-implementation word in
authoring code.

- **Identity is the name** (locked, brief 12 — names are identity). An UNNAMED surface does not
  retain. 12b M3's ordinal+signature identity is unavailable here because deriving it requires
  authoring to run, which is the thing being skipped. Confirmed acceptable with the user: only
  SURFACES need names, not every element — `element()` stays anonymous for decorative rows, and the
  surfaces that matter (panels, popups, console, lens) are already named. The one caller this bites
  is `author_nameplates`, whose plates are anonymous floating text; they do not retain today anyway
  (see the PRECONDITION under M0), so naming them is an app-side change with no deadline.
- **A surface gets everything else for free.** 12b M1/M2 already give a surface contiguous nodes, an
  independent layout unit, and placement separate from content; 12b M3 already retains its box
  column. Panels, nameplates, popups and the console are all surfaces, which is the overwhelming
  majority of what is worth retaining. Non-surface regions (a table body inside a panel) are strictly
  harder and are deliberately LAST.
- **The unsafe path self-declares.** A surface whose authoring uses `bind(obj, &T::member)` — the
  documented immediate-only escape hatch — auto-demotes to immediate. The framework can SEE that call,
  so brief 12's documented rule becomes enforced rather than remembered.
- **`.immediate()`** is the explicit opt-out, for a surface known to read state nothing tracks.
- **Retention is ON by default, deliberately.** The alternative considered was shipping M1 opt-in via
  `.retained()` and flipping the default later. Rejected: the only argument for opt-in was the
  silent-staleness hole, and defaulting ON is the better way to FIND that hole — verify mode then
  exercises every surface in the app on the first run, rather than the one panel I happened to pick.
  If the model cannot survive being the default, M1 is when we want to learn that.
- **A moving region stays clean.** Placement is not content (12b M2), so a nameplate that drifts
  every frame re-reads nothing and re-authors nothing — it writes one placement. This is the
  compounding payoff of M2 and the reason it had to come first.

### Animation, which is where the naive design breaks

`Motion::animate()` is called DURING authoring, and `Motion::end_frame()` erases every entry not
seen that frame. So a skipped closure silently drops its animation state — the entry ages out, and
next time it runs it "first-sights" and snaps.

Resolution, in two parts:
1. **An animating region is never clean.** Motion values change on their own, with no handle write
   to observe, so a region with a live transition must re-author. This is correct rather than a
   concession: its output genuinely differs every frame.
2. **Aging must learn about retained regions.** A retained region's Motion keys must be marked seen
   without running the closure, or the state dies while the region is legitimately clean.

Paint-only animation is the interesting case and it is already answered by 12b's input classes:
a hover fade re-authors its own region (cheap, one panel) and does NOT dirty layout (M3 excludes
paint from the signature). The two skips compose exactly as M0 and M3 do.

### What has to change in `layout_builder`

The largest single piece, and the one to scope carefully: **`element.text` is a per-frame index.**
`clear()` wipes `texts_` and `add_text` re-pushes, so retained nodes carry stale indices into a
rebuilt side-table. Either the text/image runs become region-owned, or the tables become persistent
with per-region ranges. Everything else (splicing a retained node range into the frame's pool,
patching parent/sibling links by offset) is mechanical by comparison.

## STOP — THIS BRIEF'S PREMISE WAS MEASURED ON A `-O0` BUILD (2026-08-04)

**`packages.demo` set `mesonBuildType = "debug"`, which meson makes `-O0`.** Every number below, and
every number in 12b, came from an unoptimized binary. Rebuilt as `debugoptimized`, same scenes, same
harness:

| | `-O0` (as measured below) | `-O2` |
|---|---|---|
| realistic shell (0 plates), total | 0.234ms | **0.033ms** |
| — authoring | 0.135ms | **0.014ms** |
| — pack | 0.081ms | 0.013ms |
| 2000-plate stress, total | 6.16ms | **~0.95ms** |

Authoring is **82 ns/node** in a real build, which is squarely in the range a Clay-style immediate-mode
pass is expected to hit. The UI costs **0.2% of a 16.6ms frame**. There is no UI performance problem,
and this brief exists to solve one.

**Consequences, recorded so this is not reopened on the old premise:**

- **M1 is not worth doing.** An app-wide read-tracking API plus a migration of every authoring site,
  to save ~0.014ms/frame. The blocking issue found earlier (`bind()` is a write path) is real and
  still true, but it no longer matters.
- **M2a's win is negligible in a real build.** prep was 0.575 -> 0.199ms at `-O0`; at `-O2` it is
  0.000-0.021ms. The change is KEPT — deleting an O(N log N) sort that existed to find two distinct
  values is right at any optimization level — but its measured justification does not survive.
- **M2b's verdict is no longer evidence.** "A splice costs about what an emit did" was measured where
  both were slow. It stays reverted because there is nothing left to optimize, NOT because it was
  proven wrong.
- **M3 is not worth doing either**, on the same arithmetic.
- **12b's milestones** keep their algorithmic value (skipping layout for a clean surface is a real
  saving at any -O level); their MAGNITUDES are `-O0` numbers and should not be quoted.

**Process lesson, which is the durable part.** Six ablations, three sentinels and a full
build-measure-revert cycle ran before anyone checked what the compiler was doing to the binary being
measured. Before optimizing anything, confirm the build is optimized — and put the optimization level
in the measurement tool's output so it cannot be forgotten again.

**Status: CLOSED as premature.** `tools/ui_cost.sh` and its `[ui-cpu]` instrumentation stay; they are
how this was found and how a real regression would be. Reopen only if a profile of an OPTIMIZED build
shows the UI is material.

## Milestones

Each must pay for itself and be load-bearing for the next — the same rule as 12b and the render-graph
arc. No scaffolding.

### M0 — Measure the whole frame, then fix scope — **DONE** (2026-08-03)

`UIPass` gained a permanent per-phase breakdown on the existing `[ui-cpu]` rolling log — author /
layout / pack / upload, plus node, surface and skipped counts. Four `steady_clock` calls a frame,
kept rather than reverted: the combined number could say the UI costs N ms but never which of the
four to attack, and brief 14 wants inline per-pass timings anyway.

Measured in the REAL demo (`STRING_SCENE=ui`, screen `all`, `STRING_FIXED_DT=0.016`), 300-frame
rolling averages, one run per scale:

| plates | nodes | total | author | layout | pack | upload | skipped/surfaces |
|--------|-------|-------|--------|--------|------|--------|------------------|
| 0 (realistic shell) |  172 | 0.236ms | 0.133 (56%) | 0.021 ( 9%) | 0.079 (33%) | 0.001 (0.4%) |   49/51   |
| 500                 | 1402 | 1.545ms | 0.807 (52%) | 0.172 (11%) | 0.559 (36%) | 0.006 (0.4%) |  824/838   |
| 2000                | 5097 | 6.16ms  | 2.95  (48%) | 0.97  (16%) | 2.20  (36%) | 0.028 (0.5%) | ~2400/3215 |

**Decisions this fixes:**

1. **Buffer retention (M2) IS in scope — the gap in the original table mattered.** Packing is a
   consistent **33–36%** of UI CPU, second only to authoring and roughly 0.7x its size. Scoping this
   brief around authoring alone would have left a third of the cost on the floor. Pack scales with
   GLYPH count, not node count (30x more glyphs at 2000 plates, 28x more pack time) — one `GpuGlyph`
   per character — so it is text-heavy UI that pays most, which is all UI.
2. **Upload needs no separate treatment.** 0.4–0.5% at every scale; three `memcpy`s into mapped
   memory. It falls out of M2 for free and is not worth a milestone.
3. **Ordering stands: M1 (authoring) first.** It is the largest phase at every scale. But M2 is no
   longer conditional — it is required for the goal.
4. **Author + pack = 85–90% of UI CPU, and both run unconditionally today.** That is the number this
   brief is against; M3's whole-frame short circuit is what takes it to ~zero.

**A precondition the real data exposed, which the benchmark could not.** The skip count SWINGS
between runs of the same scale — 1184, 2356 and 3180 of 3215 surfaces on consecutive samples — because
the demo's nameplates scale `font_px` and bar width continuously with camera distance. Those are
layout inputs, so the plates dirty as the camera moves. 12b flagged this as a possibility from the
source; M0 confirms it happens constantly in practice.

This matters MORE for 12c than it did for 12b: a dirty region there paid a relayout, whereas here it
pays full authoring AND packing. **So the HUD-scale win is gated on app-side quantisation** (step
`font_px` and bar width rather than varying them per-frame). That is a precondition to state now,
not a disappointment to discover during M1. The static-shell win — 0.236ms every frame, forever,
for a UI that is not changing — needs no app change and is exactly the energy target.

### M0 — original specification

Instrument the REAL path end to end: authoring, layout, buffer repack, upload, and the draw itself,
in the demo at several scales. The gap in the table above is deliberate — the GPU-side repack has
never been measured, and if it is the same order as authoring then M2 below matters as much as M1
and the ordering may change.

Deliverable: the table filled in, and a written decision on whether buffer retention is in scope for
this brief or its own. **This is a milestone, not a preamble** — scoping a large brief on an
unmeasured assumption is how the M2/M3 ordering in 12b went wrong the first time.

### M2a — Structural packing cost — **DONE** (2026-08-04)

**Re-ordered ahead of M1 with the user**, after checking what M1's dep tracker would actually observe
in the real app: `bind()` is a WRITE path in practice. `string-debug/src/panels.cpp` uses it only for
toggles and text fields; every display read is direct (`cv_hud_enabled().get()`, `out[i].text`,
`res.label`, formatted GPU timings). Auto-tracking would record NOTHING for those surfaces and, worse,
would default them to CLEAN — the profiler HUD would render once and freeze. The two zero-dep cases
are indistinguishable from inside the framework: a surface of literal labels (genuinely clean forever,
exactly what we want) and a surface reading `gpu_time_ms` raw (frozen). So M1's real content is
**making reads trackable at the call site plus migrating the panels**, which is a larger and more
invasive milestone than this brief described, landing on app code. Deferred pending that decision.

M2 was then measured properly before designing, by ablation (env-gated, one build, arms diffed — no
per-node clock calls to distort). Pack at 2000 plates, 2.31ms:

| part | ms | share |
|------|-----|-------|
| prep (`layer_keys` + `clip_rects`) | 0.575 | 25% |
| the node walk itself (rescanned per layer) | 0.61 | 26% |
| glyph emit (11 900 glyphs) | 0.64 | 28% |
| shape-cache lookup (~3200 text nodes) | 0.47 | 20% |
| shape emit (3617) | 0.02 | 1% |

**Half of packing was traversal, not emission** — so there was structural waste to remove before
retaining anything, needing no cache, no signature and no correctness surface. That is M2a; per-surface
retention of the packed spans is M2b and now starts from a cheaper baseline.

Done: `layer_keys` and `clip_rects` FUSED into one pre-order walk (`prep_pack`) over PERSISTENT
scratch on the pass. Both derivations depend only on the parent and nodes are pre-order, so one walk
resolves both. Two node-sized allocations a frame are gone, and so is an `O(N log N)` `std::sort` over
all 5097 keys that existed only to discover the two distinct layer values a real UI uses — replaced by
a linear insert into a set that is two entries long.

Result: prep **0.575 → 0.199ms** (-65%), pack **2.31 → 1.88ms** (-18%), UI total 6.16 → 5.79ms.

**Gate, and the part worth remembering.** Four UI screens captured at frame 300 against the pre-change
binary still in the nix store: AE=0. Then the sentinel — clipping disabled in `prep_pack` — **did not
diverge**, proving those four captures never exercised clipping at all and the AE=0 was not evidence
for the clip path. `.clip()` has only three callers (two scroll/viewport widgets, one debug panel) and
none are on the demo screens. `STRING_DAG=1` does clip, but its panel prints live GPU timings, so its
capture is nondeterministic run-to-run and cannot be an image gate either.

Settled with a direct equivalence check instead: the two original walks restored temporarily beside
the fused one, asserting agreement every 100 frames. 236 checks, zero mismatches, covering both the
clipping case (112 nodes, 16 clipping, 2 layers) and scale (1382 nodes). The check was then itself
falsified — same sentinel, 26 MISMATCH / 0 ok — before being deleted. **An image gate that cannot be
made to fail is not a gate; find the property you can actually falsify.**

### M2b — Per-surface pack retention — **BUILT, MEASURED, REVERTED** (2026-08-04)

Implemented in full and byte-identical (AE=0 on four screens), then **reverted because it does not
pay.** Recorded rather than quietly dropped: the reason it fails is a property of the packer, so any
future attempt at the same idea will hit the same wall.

What was built: a second `paint_signature` on `layout_surface` folding exactly what the layout
signature excludes (fill/stroke colour, radius, stroke width, shape, sweep, image-run parameters);
per-surface retained `GpuShape`/`GpuGlyph`/`GpuImage` streams with a per-node span table; a splice
path adding the placement delta to `rect.xy`; and global invalidation on the atlas's `generation()`
or bindless slot changing. Batching was deliberately NOT retained — keys and clips stay derived every
frame — so a retained surface splices into exactly the position it would have occupied.

Measured against M2a:

| | M2a | M2b | M2b + bypass |
|---|---|---|---|
| pack @2000 | 1.88ms | 2.36ms | 2.70ms |
| prep @2000 | 0.199ms | 0.559ms | 0.747ms |
| pack @0 plates | 0.074ms | 0.058ms | 0.060ms |

**Why it loses.** Retention removes the shaping-cache lookup and the `GpuGlyph` construction — 48% of
pack by M2a's ablation. It CANNOT remove the two things that bracket them: the per-node walk (batching
is re-derived every frame) and the copy into the frame's contiguous streams (the ring needs the data
packed in draw order). So a clean surface still walks and still copies; a splice costs about what an
emit did, and the recording of a DIRTY surface costs strictly more than not retaining at all — a
second write of its vertex data plus a span table.

A `dirty_streak` bypass was added so chronically-changing surfaces stop being recorded after 4 frames.
It made things worse (third column), because at nameplate scale the churn of arming and disarming
thousands of small surfaces exceeds what it saves.

**The generalisable finding: per-surface is the wrong GRANULARITY for packing.** On the realistic
shell, pack is only 0.079ms of a 0.234ms frame, so perfect retention has a 0.079ms ceiling and this
design reached 0.016ms of it. The cost that retention can actually remove — walk, copy AND upload —
only disappears if the whole frame is skipped, which is M3. That makes M3 the milestone to do, not a
later refinement, and it does not need any of this machinery.

Reverted with it: `paint_signature` and its `pack_color`/`bit_cast_f32` helpers. M3 will want a
per-frame equivalent, but building it now would leave unused machinery in `layout.hpp` — the thing
this brief's milestone rule exists to prevent. The design above is recorded here and is cheap to
re-derive.

### M1 — Retained surfaces

Dep tracking (record handle reads; re-read and compare), region-owned text/image runs, node splicing,
the Motion resolution above, and `STRING_UI_VERIFY_RETAINED=1`. Scoped to regions that ARE surfaces,
which is panels + nameplates + popups + console — the bulk of the win, on machinery 12b already
built.

Gate: byte-identical captures and layout dumps vs `STRING_UI_VERIFY_RETAINED=1`; authoring cost of a
static panel drops to ~the dep re-read; gtests for a stale-read being CAUGHT by verify mode (the
whole safety argument rests on that test existing and being seen to fail).

### M2 — Buffer retention — **IN SCOPE** (decided by M0: packing is 33–36%)

A clean surface's shape/glyph/image ranges are reused rather than repacked. The ranges are per
surface and contiguous for exactly the same reason the box column is, so this rides 12b's machinery
rather than adding a second notion of "region". Upload rides along for free (0.5%, three memcpys).

### M3 — Whole-frame short circuit — the energy endgame

If no region is dirty and interaction produced nothing, skip the UI pass entirely and reuse last
frame's buffers. This is the milestone the goal statement is actually about: a static UI over a
moving scene costs one comparison per region and nothing else.

### M4 — Non-surface regions

Retained regions inside flow (a table body inside a panel). Hardest, least valuable, explicitly last.

## What we are deliberately NOT taking

- **A retained CORE.** Immediate mode stays the core (locked, brief 12). Authoring is still a closure
  re-run from scratch; a region is a MEMO over that closure, not a scene graph. `if` and `for` never
  become tree mutations. Retention being ON by default does not change this — the skipped path must
  produce exactly what the run path would, which is what verify mode asserts.
- **Version counters on handles.** Value comparison needs no change to `accessor_handle` and cannot
  drift from a missed bump.
- **A general reactive/signal graph.** Regions are a flat set with a dep list each; there is no
  dependency DAG, no propagation order, no glitch problem, because a region's only output is "run or
  don't".
- **Retiring `bind(obj, &Obj::member)`.** It stays legal on the immediate path — it just may not
  appear inside a retained region, which verify mode will catch.

## Questions, all settled before M1

1. ~~Does `u.retained(...)` wrap a body, or does `u.panel(...)` grow `.retained()`?~~ **SETTLED with
   the user 2026-08-04: neither.** A named surface retains, inferred, no keyword — see "The model"
   above for the API and for why both sketched forms were rejected.
2. **First frame and resize: always dirty.** A region never seen before has no recorded deps, so it
   must run — there is nothing to compare against. Resize invalidates every region, for the same
   reason 12b M3 folds `available` into the signature: a region's output can depend on the space it
   was given without reading a single handle. Cheap and correct; a resize already relayouts
   everything. Decided rather than asked — it is the only behaviour that is sound, and the
   alternative (trying to prove a region is width-independent) is a much larger feature.
3. **Verify mode is release-available, behind the env var**, matching `STRING_UI_NO_SKIP` exactly. A
   cache you can only audit in a debug build is one you cannot audit on the machine where the stale
   panel appeared. The cost is a branch on a cached bool per region.
