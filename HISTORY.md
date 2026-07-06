# String — A History Compendium

A consolidated design history of the String renderer across its three on-disk trees
(`string_old`, `string_new`, `string`) and their branches. The goal is to record the
major decisions and turning points as the project was repeatedly rewritten, ahead of
coalescing the best parts into the current `string/` tree.

## The one surprise up front: it's (almost) all one lineage

`string_old` and `string` are **not** two separate projects — they're the same git
history, cut and continued. `string_old`'s last commit is **2022-12-04**; the `string`
repo's history begins **2022-12-06**. The `string` repo carries the entire trunk from
late 2022 through 2026. `string_old` is just the preserved *first year*, and
`string_new` is a **2024 untracked side-experiment that never rejoined the trunk**.

The age question is settled definitively:

- `string` (`rewrite-6`) is newest — its real work runs **2025-06 → 2025-09**, plus a
  2026 consolidation commit.
- `string_new` (**2024-07/08**) is *older* than `rewrite-5`, not newer. It was a parallel
  throwaway during a 2024 lull, never version-controlled.
- The misleadingly-messaged `2026-03-20` "unpushed changes" commit is real and important —
  it's the single biggest architectural jump in the whole project (see Era 3).

## Timeline

```
2022-01 ─ string_old: TUTORIAL ERA (Brendan Galea / Vulkan Tutorial)
   │        triangle → vertex buffer → push constants → 3D → camera →
   │        keyboard controller → models → vertex lighting → event system
2022-12 ─ switch Make→Meson, update Vulkan  ── trunk continues into `string` repo ──►
   │
2022-12 ─ string(master): ECS ERA
2023-11 ─   vkbootstrap, "Move ECS to its own folder" (Austin Morlan ECS)  ◄── master DEAD-ENDS here
   │
2024-07 ─ string_new: UNTRACKED PARALLEL REWRITE (no git)
2024-08 ─   flat layout, HelloTriangle, adds asset/filesystem/render_pass, Nix  ◄── abandoned
   │
2025-05 ─ string(rewrite-5): MODERN GPU-DRIVEN REWRITE
   │        Nix flake + iGPU fix → Device class + VMA → refactor swapchain/pipelines →
   │        "draw a circle"/"render shape" → "graph" → "compute + slang" →
2025-07 ─   WINDOWING FROM SCRATCH (drop GLFW/SDL → raw Wayland)
2025-09 ─   final tweaks  ◄── rewrite-5 tip (45a76ac)
   │
2026-03 ─ string(rewrite-6) = rewrite-5 + ONE commit: THE RENDER-GRAPH DUMP  ◄── HEAD, current target
```

## Tree summary

| Tree | Git? | Active span | Character |
|------|------|-------------|-----------|
| `string_old` | yes (34 commits; branches `master`, `rewrite`) | 2022-01 → 2022-12 | Learning-from-samples era. Classic Vulkan-tutorial / Brendan-Galea structure. |
| `string_new` | **no git — snapshot only** | files 2024-07 → 2024-08 | Flatter, simpler restart. HelloTriangle-level, Nix-based. Adds `asset`, `filesystem`, `render_pass`. Never merged. |
| `string` | yes (77 commits; branches `master`, `rewrite-5`, `rewrite-6`) | 2022-12 → 2026-03 | The trunk. Culminates in a GPU-driven render/task-graph engine. `rewrite-6` is HEAD and the coalescing target. |

## Era 0 — `string_old`, the tutorial era (2022-01 → 2022-12)

Learning Vulkan by following samples. The commit arc is textbook: `attempt to draw
triangle. needs lots of debugging` → vertex buffers → push constants → 3D → camera →
`add keyboard controll` (Brendan Galea's `keyboard_movement_controller`) → models →
basic vertex lighting → a hand-rolled event system. Classic OO design with a
`core / render / platform` folder split and per-object Vulkan wrappers (`vulkan_device`,
`vulkan_swap_chain`, `vulkan_pipeline`, `vulkan_model`). Late 2022 it switched
**Make → Meson** and bumped Vulkan. Two branches: `master` and a `rewrite` seed — that
seed is what became the `string` repo.

## Era 1 — `string` (`master` branch), the ECS era (2022-12 → 2023-11)

Continues the trunk. Early 2023 is "mashing random pieces together to see what works."
The defining moves land Nov 2023: **`Refactor to use vkbootstrap`** (stop hand-rolling
instance/device selection) and **`Move ECS to its own folder` / `making ECS stuff work`**
— the Austin Morlan ECS from the README's inspirations. **This branch dead-ends at
2023-11-24** and is the *only* place the ECS work lives — a prime candidate to reclaim
later.

## Interlude — `string_new`, the untracked parallel rewrite (2024-07 → 2024-08)

The one tree with no git. A flatter, simpler restart (no `core/render` split):
`main.cpp`, `renderer`, `render_pass`, `vulkan_device`, `vulkan_swap_chain`,
`vulkan_pipeline`, plus new **`asset`** and **`filesystem`** abstractions. HelloTriangle-
level, Nix-based (its `string_runtime.log` shows nix-store ICD paths). It was
contemporaneous with `rewrite-5`'s 2024 "Light refactoring" lull, never merged, and
superseded when the trunk resumed in 2025. Its `asset`/`filesystem`/`render_pass` ideas
plausibly informed the later rewrite.

## Era 2 — `rewrite-5`, the modern GPU-driven rewrite (2025-05 → 2025-09)

Branched off the ECS `master` tip and became the serious modern engine. Thread by thread:

- **Environment:** `add nix flake, fix issue with integrated graphics` (2025-05).
- **Memory & structure:** consolidate logic into a `Device` class, **integrate VMA** for
  buffers/images, refactor swapchain and pipelines (2025-06).
- **First real rendering:** `draw a circle` → `render shape` (2025-06).
- **Render graph seed:** the commit literally named `graph` (2025-06-12) — first
  appearance of the `notes.md` TaskGraph vision.
- **Shaders:** `compute + slang` (2025-06-13) — adds the **Slang** compiler and compute
  alongside GLSL.
- **Resources:** resource/file paths, split build (2025-06).
- **WSI from scratch:** `window from scratch` / `oh we're windowing now` (2025-07) —
  **drops GLFW and SDL for a hand-written Wayland client** (`platform/wayland/{client,
  window,presenter}`; the `glfw_window.cpp` / `sdl_window.cpp` remain as the abandoned
  prior attempts).
- **Build:** now **Zig + Meson** (`build.zig`, `old_build.zig`, `meson.build`).
- Ends Sept 2025 with candid tweaks (`i think it freezes my computer tho`).

## Era 3 — `rewrite-6`, the render-graph consolidation (2026-03 dump)

`rewrite-6` is **`rewrite-5` plus exactly one commit** — the long-uncommitted local work
pushed in 2026. Despite the throwaway message, it's the largest architectural leap in the
project. It **deletes the monolithic `swapchain.hpp` and guts `renderer.hpp` (~209 lines)**
and replaces them with the GPU-driven architecture from `notes.md`:

- **`render_graph`** + a **`passes/`** system: `hello_triangle`, `hello_slang`,
  `geometry`, `grid_2d`, `ui`.
- Matching **`pipelines/`**: `2d`, `3d`, `grid_2d`, `grid_3d`, `hello_slang`.
- New Vulkan core: **`driver`**, reworked **`presenter`**, **`descriptor_allocator_growable`**
  (bindless-style), **`resource_allocator`**, **`command_recorder`**.
- Core utilities: **`signals`**, **`profiler`**.

This realizes the `notes.md` TaskGraph / GPU-driven vision. It is the current HEAD and the
coalescing target — and **not in a working build state**.

## Cross-cutting evolution threads

| Concern | Progression |
|---|---|
| Build system | Make → **Meson** (2022) → **Meson + Zig** (2025) + **Nix flake** (2025) |
| Windowing (WSI) | GLFW (Galea) → SDL → **raw Wayland from scratch** (2025-07) |
| Device/instance init | hand-rolled → **vkbootstrap** (2023) → custom `driver`/`device` (2026) |
| GPU memory | manual allocation → **VMA** (2025-06) |
| Shaders | GLSL only → **GLSL + Slang** + compute (2025-06) |
| Descriptors | fixed sets → **growable/bindless** allocator (2026) |
| Architecture | OO tutorial wrappers → **ECS** (2023) → **render/task graph, GPU-driven** (2026) |

## Notes for the coalescing phase

- **ECS lives only on `master`** — if you want it back, that branch is the sole source.
- **`asset` / `filesystem`** abstractions trace to `string_new`; check whether `rewrite-6`
  has equivalents or should adopt them.
- **Raw Wayland WSI, Slang, VMA, render-graph/passes** are all `rewrite-6` strengths —
  keep as the spine.
- The `rewrite-N` numbering is informal/aspirational; only `rewrite-5`/`-6` (and the
  `string_old` `rewrite` seed) are actually preserved as branches. There is no pristine
  `rewrite-1..4` to recover.
- Breadcrumb worth reading before merging:
  `string-engine/test/old_string_newest_lmao.cpp` — a reference chunk already pulled
  forward.

## Design vision (from `notes.md`, current intent)

A GPU-driven Vulkan 1.3 abstraction, C++20+, Linux + Windows only. Layered as:
Platform (WSI/window, file IO, networking, audio) → `Device` (logical device, core Vulkan
work) → `Presenter` (VRR, swapchain recreation, presentation). The application drives a
`Scene` of primitives, compiled into a two-phase `TaskGraph`: record tasks + resources,
compile once to determine optimal run order and synchronization, then re-execute the baked
plan every frame. Resources (`TaskBuffer`/`TaskImage`) get a bindless index for their
lifetime.
