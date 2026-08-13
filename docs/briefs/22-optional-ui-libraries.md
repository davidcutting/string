# Brief 22 — ImGui debug shell as an optional library

Status: **DRAFT 2026-08-08 — decided with the user in conversation, not started.** Scoped after the
brief-21 alignment pass. Deps: none hard.

## What this brief decides

Made with the user 2026-08-08. Agents implement these; they do not re-decide them.

1. **The debug tools are rewritten on Dear ImGui.** Not because ImGui is better than `string-ui`, but
   because debug UI and production UI want opposite things, and serving both from one kit distorts
   the kit toward a terseness no product requirement asked for. Plus the ecosystem — ImPlot, and a
   node editor.
2. **`string-ui` is NOT touched.** It stays exactly as it is and keeps serving production UI. Brief
   12's docking/workspace system, brief 12b's frame architecture and the 5 UI dump baselines all
   stand. This brief re-authors debug surfaces only.
3. **The ImGui work splits into a bridge and the panels**, as two libraries. Different reuse units:
   someone may want ImGui wired into `frame_graph` + our input system and write their own panels, or
   want our console and cull HUD as-is.
4. **Optional per distribution.** A consumer takes `string-core` + a renderer and picks: our debug
   shell, our production kit, both, or neither. `string-debug` was already marked
   *"[optional per distribution]"* in the locked library split; this preserves that and makes it real
   by removing the `string-ui` dependency that currently makes the two inseparable.

### The port set

Named by the user, in priority order: **the lens, the F1 tab bar, the console, the logs.** Everything
else currently in `string-debug/src/panels.cpp` — the F2 profiler HUD, the F3 inspector, the F8
bindings inspector/rebinder, the frame-graph view, the pass DAG, the image viewer — is a **second
wave**, ported or dropped per the M0 table. Do not assume all of it survives; some of it exists
because it was cheap in the old kit.

### The new tooling this unlocks

- a **file viewer** (assets, cooked output, shader sources)
- a **node graph view**
- longer term, a **visual graph editor** hooking into audio, animation and the render graph itself

The third is a direction, not a milestone here. See "Where the node editor is going" below — it has
one architectural requirement that must be respected from the first node-graph commit, and two
collisions with locked decisions that need settling before it is scoped.

## What it costs

Much less than it would have with a `string-ui` rewrite in scope, but not nothing.

- `string-debug/src/panels.cpp` is **1150 lines** and `console.cpp` **237**. The named port set is
  roughly half of it: `author_menu_bar` (~80), `author_console` (~55), `author_logs` (~40),
  `author_lenses` (~135), plus `handle_toggles` / `handle_console_input` input plumbing.
- **Brief 13 (widgets) is not superseded, only narrowed.** It is still needed for the production kit;
  it is no longer on the critical path for debug tooling, which is what was blocking brief 14.
- **Brief 12's docking system stays** — it now serves production UI only. ImGui's docking branch
  serves the debug side. **Two docking implementations in one binary, accepted deliberately
  2026-08-08.** They never interact: different windows, different input paths, different libraries.
  The cost is conceptual, not technical, and the alternative — production UI reaching for ImGui
  docking — would put ImGui in a shipping build, which is the thing this brief exists to prevent.

### Doc drift to fix (brief-21 class of problem)

- `docs/briefs/README.md` listed `12-ui-panels.md` as *"(written; not started)"* when the brief says
  `COMPLETE 2026-08-01`. **Fixed 2026-08-08 with this brief.**
- `14-debug-ui-inspect.md` says `DRAFT`, but `author_dag`, `author_image` and `author_lenses` already
  exist in `panels.cpp` — brief-14 content that landed without the status line moving.

**M0 therefore inventories against the CODE, never against the briefs.**

## Library shape

```
string-debug-ui   ImGui <-> frame_graph + Input bridge. NO panels.  deps: core, imgui
string-debug      the panels, built on string-debug-ui.             deps: core, string-debug-ui
string-ui         production kit. UNCHANGED by this brief.          deps: NOTHING (preserve this)
string-client     screens.                                          deps: core, ui
```

The only edge that changes is `string-debug`: `(core, ui)` → `(core, string-debug-ui)`. `string-ui`
depending on nothing is a verified invariant of the locked split and must survive unchanged.

**Names LOCKED with the user 2026-08-08.** `string-debug-ui` names the role (the UI substrate debug
surfaces are built on) rather than the dependency, which is what the locked split was after when it
chose `string-debug` over `string-tooling`. The near-identical `string-debug` / `string-debug-ui`
pair follows the `string-asset` / `string-asset-tools` precedent already in the tree: the shared
prefix is the point — it says they belong to one another, and the suffix says which layer.

## Dependencies — LOCKED with the user 2026-08-08

- **Dear ImGui, `docking` branch.** Docking is not on master; it lives on the long-running `docking`
  branch. That is a branch pin, not a version pin, which bears on acquisition below.
- **`imgui-node-editor`** (thedmd) for M4's node graph. Do not write one.

### Multi-viewport is OFF — do not enable it

The `docking` branch ships docking *and* multi-viewport together, and multi-viewport must stay
disabled (`ImGuiConfigFlags_ViewportsEnable` unset). Two reasons, both structural:

1. It requires the `ImGuiPlatformIO` window callbacks — creating, destroying, positioning and sizing
   real OS windows — which means adopting `imgui_impl_sdl3` after all, reintroducing the second input
   path this brief deliberately avoids (see M1's input section).
2. It requires **rendering to N swapchains**. The engine has one `presenter_` and the graph declares
   exactly one `swapchain = true` persistent whose backing is late-latched from the frame's acquire.
   N swapchains means N graph executions per frame, or a second presentation path outside the graph.
   Neither is a debug-tooling-sized change.

Docking without multi-viewport gives dockspaces, splits and tabs inside the main window, which is the
whole ask. If tear-off-to-desktop is ever wanted, it is its own brief and it starts at the presenter.

### Acquisition — wraps (LOCKED with the user 2026-08-08)

Meson wraps, consistent with every other dependency in the tree. Vendoring was raised on the grounds
that wraps have been the recurring off-nix Windows blocker (libktx); the user's call is wraps, and it
keeps one acquisition mechanism rather than two. What that means concretely, since neither of these
is a wrapdb one-liner:

- **wrapdb's `imgui` tracks release tags on master, which have no docking.** Both wraps are
  hand-authored. Precedent: `sandbox/subprojects/nlohmann_json.wrap`. Hand-authored wraps are TRACKED
  IN GIT — do not mistake one for a generated redirect and delete it.
- **Pin a commit archive, not the branch head.** Every existing wrap here is a `[wrap-file]` with a
  `source_hash` over a fixed archive; `[wrap-git] revision = docking` would make the build track a
  moving branch and is inconsistent with that posture. Use the GitHub archive URL for a specific
  docking-branch commit, hashed. Same for `imgui-node-editor`.
- **Neither project ships a `meson.build`.** Both need one written into
  `subprojects/packagefiles/` with `patch_directory`, as wrapdb does for such projects. This is the
  actual work in the acquisition step — budget for it rather than expecting `meson wrap install`.
- Follow the `ktx.wrap` pattern of **probing for a system package first and falling back to the
  wrap**, so nix keeps using nixpkgs and only off-nix builds from source. That header comment is the
  model for how much to write down about a non-obvious wrap.

Known risk, recorded rather than re-argued: two hand-authored branch-pinned wraps plus two written
`meson.build` files is the configuration most likely to break the mingw cross build, and that build
has no CI. Expect the Windows package to be where this surfaces.

---

## M0 — Port inventory

No code. Read `panels.cpp` and `console.cpp` against the running engine and produce a table: every
debug surface, its line count, and **port now / port later / drop**. The four named surfaces are
"port now" by definition; everything else needs a decision.

**One gate check specific to this brief:** `tools/ui_dump.sh` runs `STRING_SCENE=ui`, and
`DebugPanels` authors into the same `layout_builder` as the ui-dev scene. Confirm whether any debug
surface appears in the 5 dump baselines at their default toggle state. If none do — expected, since
the `dbg.*` cvars default off — the baselines are unaffected by this brief and stay green throughout,
which is the main reason dropping the `string-ui` rewrite made this cheap. If any do, re-baseline
before M2, not after.

**Deliverable:** the table, and the ui-dump finding stated either way.

---

## M1 — `string-debug-ui`: the bridge

The point of this milestone is that it contains **no debug surfaces**. If a panel appears here, the
split has failed.

### Renderer: custom, not `imgui_impl_vulkan`

Write the `ImDrawData` renderer against the engine's own pipeline path (~200 lines). The stock
backend's `ImTextureID` *is* a `VkDescriptorSet` from its own pool and set layout — a second
descriptor world beside our bindless `descriptor_table`. With a custom renderer, **`ImTextureID` is
the bindless slot as a `uint32_t`**, resolved through `pc.slot(img)`.

That is not a purity argument: **the lens and the file/image viewers are the whole reason.** Both
display engine render targets inside a debug window. With bindless ids that is a `.reads(target)` on
the pass declaration and an integer in the draw command. With the stock backend it is a manually
created and manually kept-alive `ImGui_ImplVulkan_AddTexture` descriptor set per target per frame.

`ui_pass::record` is the precedent: bindless slot resolved from the pass's own declaration, push
constants, batched draws. Follow its shape.

### Graph integration

One declared pass. ImGui is a leaf — it reads nothing the graph owns except targets the caller asks
to display, and writes one colour attachment:

```cpp
fg.pass("imgui")
  .color(target)
  .raster([this](pass_context& pc) { record(pc); });
```

Load/store and the layout transition derive from the declaration; it is not the first writer, so it
loads. `derive_groups` may fold it into the preceding group — fine and wanted.

- **Declare it on the post-composite target, at 1×.** A 4× attachment would force
  `rasterizationSamples = VK_SAMPLE_COUNT_4_BIT` for no benefit. (See the ui-scene sample-count
  history — 1×↔4× transitions between scenes are a known sharp edge.)
- **Vertex/index buffers are owned by this library, not the graph.** Transients are GPU-only, and
  brief 21 D3 puts host-write pacing rings on the owner as persistents. Host writes before
  `vkQueueSubmit` are made visible by submission, so no barrier is owed.
- **Font atlas through `TransferBatch` + the declared `uploads.stream` pass.** ImGui 1.92+ manages
  textures via `ImTextureData` with backend-driven create/update/destroy; map it onto the existing
  upload path. Verify against the pinned version — this API changed recently and older material
  describes the previous model.
- **Engine lifetime, not scene lifetime.** Context, pipeline and font texture survive
  `compiled_frame::release` and a graph re-author; only the pass *declaration* is re-added per scene,
  as `ui_pass`'s atlas is handled today.

### Input: drive `ImGuiIO` from `string::Input`

Do **not** add `imgui_impl_sdl3`. It would consume raw `SDL_Event`s that
`sdl_window.cpp`/`glfw_window.cpp` currently fold into `Input`, creating a second input path beside
the one brief 17 exists to prevent. Map `Input` → `ImGuiIO` by hand: pointer position, buttons,
wheel, text input, modifier and key state.

**`io.WantCaptureMouse` / `io.WantCaptureKeyboard` route through the brief-17 context stack** — a
system-tier context pushed when ImGui wants capture — **not** as an ad-hoc bypass around `InputMap`.
One table arbitrates; a click landing in both an ImGui window and gameplay is exactly what brief 17
was built to stop. `handle_toggles`' F1/F2/F3/F8 keys move to brief-17 system actions rather than raw
`key_down` polling, which is where they should already have been.

This is the milestone's real cost. The rendering is a day; the input seam is where the time goes.

### Optionality

Meson feature option; subproject symlinked into `sandbox/subprojects/` with
`meson.override_dependency`, the established pattern. Opt-out is "don't link it, don't declare the
pass" — the app already owns every pass declaration, so no engine code branches on its presence.

**Gate:** demo runs with the ImGui demo window over both a 4× scene and the 1× ui scene, at 800×800 /
1600×1600 / 1920×1080. Validation and sync validation clean. NEEDS VISUAL VERIFY: the user should see
a crisp, correctly scaled demo window with working mouse interaction, and **no gameplay camera motion
while dragging inside it** (that is the input-seam failure, and it is the thing to look for).

---

## M2 — Port the named four

In this order, because it walks up the difficulty curve and each one proves a different part of M1:

1. **Console** — text input, command dispatch, history. Proves keyboard capture and text input.
2. **Logs** — a scrolling filtered list. Proves clipping/virtualisation for long lists.
3. **F1 tab bar** — the menu/toggle surface. Proves the brief-17 system-action rewiring, since this
   is what F1 toggles. With docking on, this is also the **host**: a full-viewport dockspace with a
   pass-through central node (so the 3D scene shows through where nothing is docked) plus the menu
   bar. Submit the dockspace before any other debug window each frame, or windows dock into nothing
   on their first appearance. This is the surface that decides the default layout, so land the
   dockspace before porting the second wave into it.
4. **The lens** — a draggable region resampling the frame through a debug view. Proves the bindless
   `ImTextureID` path end to end, and is the surface that would have been worst with the stock
   backend.

`set_hud_extra()` / `set_inspector()` change from `std::function<void(ui::Ui&)>` to
`std::function<void()>` — ImGui is global state. The slot *pattern* is unchanged and remains why this
library is renderer-agnostic; only the signature moves.

`string::ui::set_theme()` currently lets debug and client share a palette without depending on each
other. That link dissolves; ImGui has its own style struct. Decide whether `string::client::theme()`
values are mirrored into it or the two are simply allowed to differ — a debug shell that matches the
product is not a requirement.

`string-debug/meson.build` drops `ui_dep`. **Verify the dependency is gone from the built artifact**,
not merely unused in source — that edge disappearing is the point of the brief.

**Gate:** each surface demonstrated live with real data, as the library-split gate did (real cull
counters, a hovered inspector row, the isolate cvar). Complexity gate reported.

---

## M3 — Second wave, per the M0 table

The F2 HUD, F3 inspector, F8 bindings, frame-graph view, pass DAG, image viewer. Mechanical once M2
lands. Anything the M0 table marked "drop" is deleted here, not left dead.

---

## M4 — New tooling

### File viewer

Browse and inspect on-disk artifacts: assets, cooked output, shader sources. The engine already knows
where these live (`STRING_RESOURCES_DIR`, the manifest in `string-asset`, the shader search dirs) —
this surface reads that rather than introducing its own notion of a project tree. Image files display
through the bindless `ImTextureID` path M1 established.

### Node graph view — **read-only first**

A viewer, not an editor. The first target is the render graph, because it already has everything a
node view needs and nothing to invent: `frame_graph::declarations()` gives passes with their
`resource_use` lists, `compiled_frame::order()` gives the toposort, `groups()` gives the render-pass
grouping, and `ran()` gives per-frame liveness. A node view of that is a direct rendering of data
that exists.

**The architectural requirement, from the first commit:** the node view is written against a generic
graph model — nodes, typed ports, edges, plus a per-node inspector callback — and the render graph is
its **first adapter**, not its native format. A node view written directly against `pass_decl` cannot
later serve an audio or animation graph without being rewritten, and rewriting it is exactly what the
"visual graph editor" ambition below cannot afford. This costs almost nothing now and everything
later.

**`imgui-node-editor` is locked** (see Dependencies). It brings its own node/link/pin drawing,
selection, panning and — note — **its own settings persistence** for node positions, as a JSON blob
per editor context. Route that through the engine's own user-data location
(`core::user_cache_dir`, as the shader cache does) rather than letting it write next to the
executable; a package directory is read-only on a real install.

Its API is immediate-mode over stable integer ids (`ed::BeginNode(id)` …), which suits the adapter
model directly: the adapter's job is to hand out stable ids and enumerate nodes, ports and links, and
nothing about `pass_decl` leaks into the view.

---

## Where the node editor is going

Scoped later, but two collisions with locked decisions need settling **before** it is, and recording
them now is cheaper than discovering them mid-brief.

**Audio — clear.** miniaudio is the *locked* 3rd-party choice in `docs/engine-feature-spec.md` item
12, with a thin engine surface (sound handles, positional emitters, buses). A bus/effect graph is a
natural node-editor target and nothing in the spec conflicts.

**Animation — a real conflict.** The spec (item 1) locks *"engine does sampling + blending only;
state machines are game code"*. A **blend-tree** editor fits that. A **state-machine** editor does
not — it would be authoring game code in the engine, against an explicit decision. ~~Also note no
animation library is named anywhere in the spec: **ozz is a new proposal, not a locked choice**, and
picking it is its own decision.~~ **Superseded 2026-08-13: ozz-animation 0.17.0 is LOCKED (user
decision, brief 23).** The sampling/blending-only boundary in this paragraph still stands — brief 23
takes exactly ozz's SamplingJob/BlendingJob/LocalToModelJob + offline builders, nothing stateful —
and the paperdoll + shared-skeleton + animation-LOD constraints are carried as format hooks there
(`palette_offset` sharing, per-part `CookedSkin`, separable `.anim` packs).

**The render graph — the sharpest one.** Brief 20 locked **AUTHORED ONCE, COMPILED ONCE**: a toggle
is an in-graph conditional, a resize swaps backing, and neither re-authors or re-plans. The only
sanctioned mutation is `frame_graph::clear()` + re-author + recompile, which is what a scene switch
does. So a visual render-graph *editor* is one of:

- a **viewer** with live toggles (what M4 proposes — toggles already exist as `enable_fn`);
- an **authoring tool that emits a re-author** — edit, then clear/re-declare/recompile wholesale,
  reusing the scene-switch path;
- **live graph mutation**, which contradicts the locked model and would need the user to reopen it.

The second is achievable and the interesting one. It is not this brief.

---

## Conventions

House rules apply (`docs/briefs/README.md`): `nix flake check` green; new files need `git add -N`;
perceptual gates at multiple resolutions including a non-square one; never claim visual success —
report **NEEDS VISUAL VERIFY** with what to look for and what failure looks like. Run
`tools/complexity.sh --gate` and report the delta.

## Open questions for the user

1. **Second-wave scope**: does everything in M3 survive, or is some of it (the DAG view, say,
   subsumed by the M4 node graph) better deleted than ported? Answerable only from the M0 table, so
   it waits for M0 rather than for the user.

*Resolved 2026-08-08, all with the user: library name **`string-debug-ui`**; ImGui **docking
branch** + **`imgui-node-editor`**; multi-viewport **off**; acquisition by **meson wraps**. The
brief is fully specified — nothing blocks a start but the decision to start.*
