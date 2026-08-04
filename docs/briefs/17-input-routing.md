# Brief 17 — Unified input routing

## Why this is one system, not two

Gameplay needs low latency and remapping. The UI needs routed, consumable events. It is tempting to
build those separately, and it is wrong: the player rebinds "cast fireball" and "open bags" in the
same menu, from the same profile, and the two must arbitrate with each other — chat has to swallow
WASD, a modal has to take Escape, and nothing may swallow release-mouse-look.

The unification point already existed. `InputMap` was already a remappable action layer, and
`Input::text_capture` was already a UI-consumes-input mechanism — just with one claimant, one
priority, and all-or-nothing. This brief generalises that flag rather than adding a second system
beside it.

**What is shared:** the binding table, the action vocabulary, and arbitration.
**What stays different:** delivery.

- **Gameplay is POLLED.** `map.held(action)` is an integer lookup plus one bit. No allocation, no
  event dispatch, and — load-bearing — no dependency on the UI's layout tree. Nothing in `InputMap`
  walks a tree.
- **UI is ROUTED.** "Which of three open panels gets Escape" is not a question polling can answer.

These are two views of one resolved action set: the UI sits higher in the stack, takes its turn, and
gameplay reads the remainder.

Frame order, which is where the latency lives:

```
poll raw device state
resolve chords -> actions           (shared binding table)
UI arbitration: offer to the context stack, mark consumed
gameplay polls the remainder        <- integer lookup + a bit, no tree
simulate
author UI -> layout -> resolve interaction (for NEXT frame)
```

### The stack is app state, not tree state

Deliberate, and it is the decision most likely to be second-guessed later.

UI focus is resolved post-layout, so anything derived from the element tree is one frame stale. If
contexts were layout nodes, "is chat open?" would be stale too — and one frame of movement keys would
leak into the character on the frame chat opens. Push/pop is the app saying so *now*.

Focus *within* a UI scope can still be tree-derived; that staleness is the same one hover and press
already live with, and it never reaches gameplay. (An earlier draft of this design had scopes as
layout nodes. The gameplay-latency question is what exposed the flaw.)

---

## M1 — Contexts + interned action ids — **DONE** (2026-08-03)

### Interning

`ActionId` (`string-core/include/string/platform/action_id.hpp`) is a hashed name behind a strong
type — an action id, a context id and an entity id are all 64-bit hashes, and the compiler should be
what notices when they are mixed up. `"jump"_action` is consteval, so a query site carries no string
at all; `action_id(name)` is constexpr for computed cases.

Hashing moved from every query to bind time (or compile time). Queries key on `uint64_t`: no string
hash, no string compare. `std::unordered_map` is ample at the tens-of-actions scale this runs at; a
flat sorted vector is the known next step if action counts ever make node-chasing visible.

**Bind by name, query by id.** The string `bind_*` overloads intern the name *and remember it* — that
is what the rebinding UI and debug output read back via `name_of()`. Query sites use interned
constants (`camera_actions::move_forward`, `debug_actions::toggle_lod`), so a typo is a compile error
rather than an action that silently never fires. Stringly-typed queries are silent in both directions,
which is the actual reason to do this, ahead of the cycles.

`ActionId{}` (hash 0) is the "no action" sentinel and is rejected by bind and query, so a forgotten
initialiser cannot alias a real action.

### The context stack

Contexts are pushed/popped by the app and form a stack. Each claims either **everything** below it
(exclusive — a modal, the console) or **a specific list** of actions (chat taking the text keys and
Escape while leaving the camera alone). A query names the context it reads as; the default is
`base_context()`, so gameplay and debug code read exactly as they always have.

An action reaches a reader unless some context *above* it claims that action.

Decisions worth keeping:

- **`pop_context` removes by identity, not position.** Surfaces close out of order.
- **Re-pushing an active context raises it** rather than duplicating it, so "focus this panel" and
  "open this panel" are the same call.
- **Popping the base context is a no-op.** It is structural; removing it would leave every reader
  dangling.
- **An inactive reader receives nothing** rather than falling back to base. Falling back would hand a
  closed surface live gameplay input; receiving nothing is both correct and an obvious symptom.
- **`blocked()` is public.** A keybind UI that greys out a suppressed binding, or a HUD that dims a
  claimed hotbar slot, needs to *show* suppression rather than merely experience it.
- `ScopedContext` for RAII push/pop.

### text_capture, restated rather than replaced

`Input::text_capture` keeps its meaning and its callers (the console still just sets it). Inside
`InputMap` it is no longer an early-out: it raises the implicit `InputMap::text_context()`, expressed
in the stack's vocabulary rather than special-cased beside it.

> **Corrected in M2.** M1 placed this context above the WHOLE stack. That is wrong, and wiring the
> first real consumer proved it — see "M1 CORRECTION" below. It sits directly above BASE: text
> capture suppresses gameplay, which is what it has always claimed to do.

Text itself stays out of the action system entirely. You do not rebind `e` to mean `e`, which is why
the flag lives on raw `Input` rather than being a binding.

### Verification

15 tests in `string-core/test/input_map_test.cpp` covering interning, selective vs exclusive claims,
stack priority, out-of-order popping, raise-on-repush, inactive readers, RAII, and the two
text-capture behaviours. Test execution in the check was confirmed by deliberately breaking an
assertion and observing the derivation fail, then reverting.

Gates: `checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI baselines byte-identical.

---

## M2 — UI routed events + focus scopes — **DONE** (2026-08-03)

### The constraint that shaped it

`string-ui` depends on nothing — not on string-core, so not on `ActionId` — and `interaction` is a
plain value type by a locked brief-12 decision. So the kit could not consume the engine's action ids
directly, and it should not: it has no business knowing what a key is.

The kit therefore owns a small closed vocabulary (`ui_action`: activate, cancel, submit) and receives
intent BITS. The host resolves those from `InputMap` in `populate_interaction` — "THE ONE CROSSING",
already the only place platform input becomes UI vocabulary. Remapping is real (bindings live in the
shared map, named `ui.activate` / `ui.submit` / `ui.cancel`) and the kit stays dependency-free. This
is the same split `nav_x` and `primary_pressed` already used, extended to what were hardcoded key
checks.

### Routing

`element.actions` is a bitmask of what an element HANDLES. An action walks from the focused element
up its ancestors; the first that declares it receives it, stopping at a modal scope. With no focus,
it goes to the topmost handler by the same layer key the renderer and hit test use — which is what
makes Escape close the frontmost surface without having tabbed into it first.

The result is one `{action -> target id}` table on `interaction`, resolved post-layout, read by
widgets as `ia.took(id, action)`. **Consumption is structural**: exactly one target exists, so there
is no second claimant to lose a race to. Same arbitration argument as the wheel, and for the same
reason — with a field inside a dialog inside a panel, "whoever checks first" is not a rule, it is a
bug that depends on authoring order.

Targets clear every frame. A stale one would re-fire a dialog's cancel forever after the key came up.

Routing runs AFTER nav, so pressing a direction and activate on the same frame activates what you
navigated to.

### Focus scopes

`element.scope` confines directional nav to a subtree — this is what stops the stick walking out of
an open dialog into the buttons behind it. `element.modal` additionally swallows unhandled actions.

**These are NOT the engine's context stack**, and conflating them would be the mistake. The context
stack is coarse arbitration between whole surfaces and must be immediate app state (a frame-stale
"is chat open?" leaks a frame of WASD into the character). Focus scopes are fine routing within the
UI, where a frame of staleness is the same one hover and press already live with and never reaches
gameplay.

### Wired, not just built

The debug console is now `.modal().handles(ui_action::cancel)`: Escape closes it, and nav cannot
leave it. `TextField` declares `.handles(submit)` and reads `took()` instead of a global flag.
`InputMap` gained gamepad-button binding, which controller-first UI needed and did not have.

### M1 CORRECTION found by wiring that consumer

M1 placed the implicit text context above the WHOLE stack. Wiring Escape-closes-console exposed the
flaw immediately: the console raises `text_capture`, which then blocked the very action that closes
it. The workaround would have been to keep reading the toggle key raw — which the console already
does for backtick, and which is precisely the special case this brief exists to remove.

`text_capture`'s contract has always been that typing must not drive the camera: it suppresses
GAMEPLAY, which reads at base. So it now sits **directly above base**, and anything the app
deliberately pushed higher is unaffected. The M1 test asserting the opposite
(`TextCaptureOutranksEvenAPushedModal`) is replaced by
`TextCaptureSuppressesBaseButNotContextsPushedAboveIt`, with the reasoning recorded in place.

The UI reads its actions as its own `ctx.ui`, pushed above base once and left there. Two consequences,
both wanted: gameplay keeps receiving anything the UI does not claim, and a capturing text surface
does not stop the UI acting on its own Escape.

### Verification

8 new tests: focused-element delivery, bubbling to an ancestor, modal swallowing, topmost-handler
fallback with no focus, target clearing, nav confinement, nav unconfined outside any scope. Execution
was confirmed by pointing one assertion at the wrong element and watching that specific test fail.

Gates: `checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI baselines byte-identical.

### Still hardcoded, deliberately

`nav_x`/`nav_y` remain host-derived intent rather than bound actions. They are already abstract (the
host owns stick-flick and d-pad conventions), so routing them through the map would be churn for
little gain. `backspace` and `typed_text` stay out for the stated reason: text is not an action.

## M3 — system actions, per-surface claiming, rebinding UI — **DONE** (2026-08-03)

- **Unconsumable actions.** Release-mouse-look, push-to-talk, screenshot. The mechanism exists (bind
  them in a context above `ctx.ui`); nothing does it yet, and it is cheap now / ugly to retrofit once
  surfaces start claiming broadly.
- **A rebinding UI.** `name_of()` and `blocked()` were built for it: the first names an action for a
  human, the second lets the UI SHOW that a binding is currently suppressed rather than merely
  behaving as though it were.
- **Per-surface claiming.** Only the console claims anything today (via text_capture). Chat taking the
  text keys while leaving the camera alone is the next real case, and is what `push_context(id,
  actions)` was built for.

## Deferred

- **Sub-frame input timestamps.** Everything here is frame-granular, which is standard and correct
  for an MMO client. If competitive responsiveness ever demands it, SDL provides timestamps and the
  place to use them is the raw -> action resolve step, which is already a distinct stage. The
  architecture does not preclude it.
- **Chords/modifiers** (Ctrl+Shift+K). The binding table stores flat keys today.
- **Binding persistence** to a profile.

---

## Two defects from the first live run (2026-08-03)

User: "esc doesn't close the console." Two bugs, only one of which they had noticed.

### 1. A two-way CVar mirror stomped the close

The reported symptom, and nothing to do with routing — the kit-side path was correct, which a test
mirroring `author_console` exactly confirmed before anything was changed.

The console's open state lives in two places: the `console_open_` member and the `dbg.console` CVar
(so a headless capture or a console command can drive it). `handle_toggles` reconciles them
**CVar-first at the top of every frame**. Escape's close set only the member, so the next frame's
reconcile saw the CVar still saying "open" and put it straight back. The grave key escaped this only
because it toggles and writes the CVar inside the same call.

Fixed structurally rather than by adding a second write at the call site: `set_console_open()` is now
the only writer, and both mirrors go through it. Two mirrors need one writer.

### 2. The UI's context was pushed EXCLUSIVE, killing all gameplay input

Worse, and not noticed yet. `push_context(id)` is the exclusive overload — it claims everything below
it. `ctx.ui` sits above base, so pushing it that way put a blanket claim over base and silently
suppressed **all** gameplay input: camera, and every debug key. The comment directly above the call
asserted the opposite ("gameplay keeps receiving anything the UI does not claim").

The context exists to give the UI a POSITION on the stack to read from, not to take anything; what
gets claimed is a per-surface decision. It now pushes with an empty claim list.

**No gate could have caught this**, which is the part worth remembering: captures drive the camera
from `STRING_CAM`, and layout dumps do not use gameplay input at all, so both stayed green while
every key was dead. `InputMap.AContextWithNoClaimsTakesNothingFromBase` now pins the distinction
between the two overloads; it was confirmed to fail against the buggy version before being kept.

The general lesson for this brief: a claim that is too BROAD fails silently from the claimant's side.
The UI kept working perfectly while holding everything else's input hostage. Anything that pushes a
context should state what it claims, and `push_context(id)` with no list should read as a deliberate
"I am modal", never as the convenient shorter overload.

---

## M3 detail

### PRIORITY, because a stack cannot say "never swallow this"

Writing the first system-action test exposed the gap immediately: `ctx.system` is pushed at startup,
and in a plain stack **whatever is pushed later sits higher** — so a dialog opened at any point
afterwards would take the screenshot key away from the player, and the dialog would look entirely
correct from its own side.

Contexts therefore carry a priority and the stack is kept sorted by it, push order breaking ties
only. Three tiers: `kBasePriority` (gameplay), `kSurfacePriority` (panels, dialogs, chat — the
default), `kSystemPriority` (unswallowable). Ties still favour the later push, which is what makes a
newly opened surface outrank an older one at the same tier.

`InputMap.PriorityIsIndependentOfPushOrder` runs the same scenario both ways round, because the whole
guarantee is that timing does not matter.

### System actions

`ctx.system` is pushed once at the system tier, claiming **selectively**. That is deliberate and is
the same trap as the M2 defect: the exclusive overload here would suppress every gameplay key in the
game, from above, silently. A system context takes its own actions off everyone below and nothing
else.

`system.screenshot` (F12) is the first real one. It sets `r.capture.frame`, which the renderer now
reads **live** rather than latching at construction — one mechanism serving both the headless gates
(which set it from the environment) and a runtime trigger. The two capture paths that briefly existed
were collapsed into one; the exterior gate stayed AE=0 across the change, which is what proves the
live path works, since the gate itself now runs through it.

### Per-surface claiming, with a real consumer

The rebinder needs it: while it is waiting for a key, it pushes an **exclusive** context, so pressing
W to bind it does not also walk the camera and pressing Escape does not close the panel you are
binding from. That is per-surface claiming doing work rather than demonstrating itself.

(The originally-imagined consumer — chat claiming text keys while leaving the camera alone — has no
implementation to attach to: the chat screen is a radial ping menu, not text entry. Building a
claimant for a mock would have been speculative, so it waits for real chat.)

### The binding inspector / rebinder (F8, `dbg.bindings`)

What `name_of()` and `blocked()` were built for. Per action: the name a human knows it by, every
source it is bound to (key, mouse and pad — showing only the first would misreport the others as
absent), and whether a context above base is **currently** taking it. That last column is the one
that is otherwise invisible: a binding that looks correct and silently does nothing.

Supporting pieces: `InputMap::actions()` enumerates, **sorted by name** — an unordered_map iterates
in bucket order, and an immediate-mode list built from that would reshuffle between frames and be
impossible to click. `key_name`/`button_name` (`input_names.cpp`) name codes for humans, as a switch
rather than a table because `KeyCode` is sparse to 347 and a table would be mostly holes, each one a
silent empty cell. `Input::first_key_pressed()` answers "bind the next key I hit" — the one question
that is genuinely about a KEY rather than an action, which is why it lives on raw `Input`.

Axis actions are listed but not rebindable: they are key PAIRS, and rebinding from a single press
would silently drop half.

### Verified

Panel rendered and read back from a live layout dump: all actions listed with real key names, axes
marked `(axis)`, and `system.screenshot F12 blocked` — correct, and the feature showing its own work.

9 new tests (priority both orderings, system-vs-later-surface, tie ordering, enumeration order and
contents, rebinding replacing rather than shadowing, key naming, first-key edge). The priority test
was confirmed to fail with the priority argument dropped.

Gates: `checks.default`/`ui`/`cook` green, exterior AE=0, 5/5 UI baselines byte-identical.

### Known and left

- The panel says an action is blocked but not BY WHOM. Reporting the claiming context needs a small
  API addition; useful, not needed yet.
- Binding to mouse or pad buttons is display-only — capture is keyboard-only so far.
- **F12 -> screenshot is the one link no gate covers.** The live capture path is proven (the exterior
  gate runs through it), but the key-to-CVar handler is only proven by reading it.
