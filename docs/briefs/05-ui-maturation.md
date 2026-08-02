# Brief 05 — UI maturation: abstraction, feel, gamepad

Status: implemented (pending user visual verify). DONE: M0 ui-dev scene (STRING_SCENE=ui; ~42-67ms
build); layout floating/overlay primitive; (1) motion system (easing lib + spring + engine-side anim
table keyed by element id; hover glide / press feedback in widgets); (2) dynamic Unicode glyph atlas
(grow/evict shelf packer, UTF-8, kerning, word-wrap, clip, crisper 48px SDF) — Cyrillic/Greek/accented
names verified in captures (CJK needs a CJK font; DejaVuSans lacks them -> '?', mechanism proven);
(3) widget kit (panel/button/progress_bar/tooltip/icon_cell + radial menu + cooldown-sweep shader
mask); (4) gamepad-from-the-start (SDL3 backend: buttons+axes+hotplug; Input gamepad state; UIPass
directional focus nav over the layout tree + focus-visible rings + A-to-activate); (5) feel
instrumentation (click->record latency log; UI sound-cue no-op hooks; [ui-cpu] rolling cost log);
(6) all four mock screens (nameplates+castbars, inventory+tooltips, actionbar+cooldowns+keybinds+
proc-glow, quick-chat/ping radial+feed+world markers) in the demo author; (7) flat/vector visual
language throughout. 500-nameplate stress holds budget: ui-dev ~3.3ms UI CPU / ~3.6ms frame; Sponza
~4.0ms UI CPU / ~8.8ms frame. flake check green (incl. engine gtests + compile-time layout
self-tests). Demo (Sponza) scene regression clean.

## Workflow (permanent UI sandbox)

`STRING_SCENE=ui ./run.sh` is the UI-dev sandbox: no GeometryPass, flat gradient background + orbit
camera + synthetic world anchors, near-instant startup. Levers (all STRING_* env / CVars):
- `STRING_SCENE=ui|demo` (dbg.scene) — sandbox vs full Sponza.
- `STRING_UI_SCREEN=nameplates|inventory|actionbar|chat|all` (dbg.ui.screen).
- `STRING_UI_NAMEPLATES=N` (dbg.ui.nameplates) — synthetic anchor/nameplate stress count (works in
  BOTH scenes; over Sponza it overlays the stress to measure UI cost on real geometry).
Iterate in the ui scene; verify against Sponza at milestone close.

(Moved from slot 07 in the 2026-07-22 reorder: the UI system now lands BEFORE
debug tooling, PBR, and VFX, so the tooling panels — console, profiler HUD,
inspector, effect editor — are built on the real abstraction instead of more
throwaway overlay code.)

## Goal

Mature the existing authored-callback + layout-tree UI into a **powerful but
simple abstraction** with a **satisfying feel**. Acceptance is a set of mock
game screens, judged by the user. Do NOT replace the architecture — the
callback + layout model (`ui_pass`, `layout_builder`, `UiContext`) is the
chosen shape; harden and extend it.

## Current state

- Unified `UIPass`: per-frame `layout_builder` tree via an `Author` callback,
  shape ring + SDF glyph ring, hover/focus/typed-text, modal game/UI capture.
  ASCII-only baked atlas; rounded-rect shader shapes; SDL backend only for
  text-input/abs-mouse (GLFW/Wayland plumbing TODO); SDF "not super crisp".

## Decisions (locked with user)

- **Visual language: clean flat/vector** — shader-drawn panels/rounded rects/
  gradients/soft shadows stay primary. Texturing is light: an icon/item-art
  atlas (bindless) — no 9-slice fantasy chrome.
- **Motion is a first-class layout property**: per-element transition
  declarations in the authoring API (one line: property, duration, curve) for
  position/size/color/opacity; easing curve library + spring option; hover
  glide, panel slide/fade, press feedback. Because the tree is rebuilt per
  frame, transitions key off stable element ids (make_id) with an engine-side
  animation state table interpolating declared targets.
- **Feel is measured**: click→visual-response latency instrumented; UI sound
  cue hooks (no-op until audio lands).
- **Text**: dynamic glyph atlas (grow/evict) replacing the baked ASCII atlas —
  required for player-name-safe Unicode; shaping stays simple (no HarfBuzz).
  Fix SDF crispness (bake larger / tune smoothstep). Kerning + word wrap +
  clip/scroll.
- **First-playtest chat model shapes the widgets**: NO free-text chat — canned
  quick-chat lines, **emoticons (inline images in text runs)**, and **tactical
  pings** (LoL-style: radial menu → world-anchored ping marker + feed entry).
  Text *input* still exists (names/search), but chat UI is selection-driven.
- **Gamepad from the start**: directional focus navigation over the layout tree,
  gamepad bindings via input_map, focus-visible styling. Radial menus (pings,
  quick chat) designed controller-first. SDL gamepad backend.
- **Widget kit includes curve-editor + color-picker widgets**: built here as
  general widgets (they exercise the interaction + transition systems); the
  debug tooling (brief 06) and VFX effect editor (brief 08) consume them.

## Mock screens (the acceptance test, in a demo "game screen" author)

1. **Nameplates + cast bars** — world-anchored (project 3D→screen), health/cast
   bars, distance scaling/fade; **synthetic 500-nameplate stress** with frame
   cost reported (raise kMaxShapes/kMaxGlyphs as needed).
2. **Inventory + tooltips** — item grid from the icon atlas, drag-drop, hover
   tooltips with layout-driven size, rarity color styling.
3. **Action bars + cooldowns** — hotbar, radial cooldown sweep (shader mask),
   keybind labels, proc-glow (transition system showcase).
4. **Quick-chat + pings** — radial menu (mouse + gamepad), emoticon text runs,
   ping markers in world + feed.

## Acceptance

- All four screens work with mouse/keyboard AND gamepad-only; transitions feel
  smooth (user judgment); 500-nameplate stress holds budget (numbers reported).
- Arbitrary Unicode player names render in nameplates/tooltips (dynamic atlas).
- Authoring stays terse — building a new panel with motion is a few lines in an
  Author callback; no retained-widget machinery leaked into the API.
- Validation clean; flake check green; NEEDS VISUAL VERIFY per screen.
