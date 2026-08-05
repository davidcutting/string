# Brief 18 — Asset import + data-driven scenes

**Status:** DRAFT — not started. Written 2026-08-04, after runtime scene switching landed.

## Why

Scene switching works: the registry lists scenes, the menu switches between them, and teardown is
leak-clean. But every scene is a **C++ lambda in `sandbox/demo_scene.cpp` with its asset paths
hardcoded**:

```cpp
registry.add("sponza", "Sponza — full scene, meshlets, CSM, GI", [atlas, np_stress](...) {
    auto geo = std::make_unique<GeometryPass>(ctx, std::vector<std::filesystem::path>{
        "assets/sponza/main/NewSponza_Main_glTF_003.gltf", ... }, mesh_stats);
    return make_geometry_setup(ctx, std::move(geo), mesh_stats, atlas, np_stress);
});
```

So an artist with a new glTF cannot get it on screen without editing C++ and rebuilding — which
defeats the point of the menu. This brief closes that: **drop an asset in, pick it from the Scene
menu.**

### The engine currently does not start without assets

Worth stating first, because it reframes the brief from a feature into a bug fix.
`sandbox/assets/.gitignore` lists `sponza` — **the assets are not in the repo.** But
`registry.add("sponza", ...)` runs unconditionally and `STRING_SCENE` falls back to `sponza`, so a
fresh clone lists a scene it cannot load, defaults to it, and reaches `bake_gltf` on a missing file.
The artist this brief exists for does not get a degraded experience; they get no engine.

The target: **clone the engine with no assets at all, and it runs.** The scene list is empty, the
engine boots, and the artist imports whatever glTF they like — Intel's Sponza included, imported by
them through the same path as everything else.

## What already exists (do not rebuild)

- **`string_cook`** — the CLI. glTF -> `.cooked` blob + KTX2/BC7 texture siblings, incremental via a
  `.cook_manifest` sidecar (size+mtime fast path, content hash on change).
- **In-process geometry cook-on-load** — `GeometryPass`'s loader cooks a missing/stale `.cooked` on
  the spot (WARN + CLI hint). A raw glTF already loads; it is only the *textures* that don't cook.
- **`cook_scene_textures(CookedScene, base_dir, params)`** — the texture cook is already a library
  function taking exactly what the in-process geometry cook produces. M2 is a call, not a port.
- **`make_geometry_setup(ctx, geo, stats, atlas, nameplate_stress)`** — the generic factory that
  turns a `GeometryPass` into the full pass set (froxel/ibl/gtao/gi/sky/hiz/phase2/debug/ui/post).
  Every geometry scene already goes through it. A data-driven scene is the same call with paths
  from a file instead of a literal.

The gap is narrow and specific. Most of the machinery is built.

## Decisions

### Nothing ships; an empty scene list is a valid state

- **No scene descriptors are committed.** Discovery finding zero scenes is normal, not an error.
- **`ui` is the fallback scene** when nothing is discovered or the requested name is unknown. It has
  no geometry and starts instantly, so it is the natural empty state — no special "no scene" mode
  needs inventing.
- **Sponza loses its privileged status.** It becomes a descriptor generated locally by importing it,
  through exactly the path the artist uses. No built-in shortcut, so the shared path cannot rot
  unnoticed.

Gate consequence, stated rather than papered over: M1's byte-parity gate is a **local** gate, since
CI has no assets. That is already true of `tools/gate.sh`, so it is not a new weakness — but the data
path's proof depends on a machine that has Sponza on disk.

### Scene descriptors are JSON, via nlohmann/json as its own wrap

`assets/scenes/*.scene.json`, one file per scene: `name`, `description`, `models` (array), `camera`.
The paths merge into one draw set exactly as the sponza literal does today.

An earlier draft chose `key=value` to avoid a JSON dependency, on the grounds that the only parser in
the tree is simdjson arriving transitively through fastgltf. **That was right about simdjson and
wrong about the conclusion.** The simdjson facts, since they are easy to get backwards: the *wrap*
path (Windows/off-nix) is the self-contained one — `fastgltf.wrap` fetches simdjson's pinned
single-header at configure time, so `dependency('simdjson', required: false)` in
`string-core/meson.build` finds nothing and skips. It is **nix** that needs a system simdjson,
because nixpkgs' fastgltf does not bundle it.

So the real objection was only ever to *piggybacking on a fragile transitive dep*. A properly
declared wrap has none of that problem: nlohmann/json is header-only, in wrapdb, and resolves via
wrap off-nix and nixpkgs on-nix — the same shape as the ten wraps already in
`string-core/subprojects/`. And JSON earns its keep as descriptors grow past a flat path list
(per-scene sun angle, transforms, multiple assets).

**Do not** reach for simdjson to avoid adding a dependency. That is the fragile path, and it is the
one that would work on nix and break for the artist.

### Texture cooking happens at IMPORT, not at load

Geometry cook-on-load is cheap enough to be implicit. **BC7 encoding is not** — cooking Sponza's
texture set is minutes, not milliseconds. If load silently cooked textures, the first load of any
new asset would look like a hang, and it would be paid inside `load_scene`'s device-idle stall.

So: **import** is the explicit, once-only, slow step (with progress in the log); **load** keeps
today's behaviour — prefer the `.ktx2` sibling, else stb. That also preserves the existing property
that cooking is incremental and optional.

Consequence to state plainly: an asset that is imported but not yet texture-cooked still loads, just
on the slow stb path with no streaming. That is the current behaviour, not a regression.

### DECIDED (user, 2026-08-04): content lives in a USER-CHOSEN folder

Not a fixed path in the repo. The user points the engine at a folder; descriptors are discovered
there and imports are written there. This removes the "every import dirties the working tree"
problem outright, and it is what makes the engine shareable — the artist keeps their content
wherever they keep their work, and it survives re-cloning the engine.

- **Paths inside a descriptor resolve relative to that descriptor's own folder**, never to the
  engine or the CWD. A content folder can then be moved, copied or handed to someone else whole.
- **v1 selection is a cvar + console command**, with an env override for headless gating. A native
  folder picker is a brief-13 widget problem, and on Linux it would mean a portal/GTK dependency
  that `string-ui` deliberately does not have. Not worth blocking on.
- **One root in v1.** A list is a plausible extension; the menu only needs to show what was found.

**This needs config persistence, which does not exist yet.** The chosen folder has to survive a
restart or the feature is a toy. `user_cache_dir()` is the closest thing and is the wrong home twice
over: it is a *cache* (documented as wipeable — losing your content root to a cache clear is a bad
day), and it is POSIX-only, reading `XDG_CACHE_HOME`/`HOME` with no Windows branch at all.

So M1 adds a `user_config_dir()` sibling and persists the content root there.

**Config and cache are different kinds of data and must not share a home** (an earlier draft of this
section ran them together):

| | size | Windows | POSIX |
|---|---|---|---|
| **config** — the content root, settings | bytes | `%APPDATA%` (Roaming) | `XDG_CONFIG_HOME`, else `~/.config` |
| **cache** — shader + meshlet content-hash caches | **gigabytes** | `%LOCALAPPDATA%` (Local) | `XDG_CACHE_HOME`, else `~/.cache` |

`%LOCALAPPDATA%` for the cache is not a style preference. `%APPDATA%` is *Roaming*: on a
domain-joined machine that profile syncs to a server at login/logout, so a multi-gigabyte cache there
is a genuine failure, not just wasted disk. Local is precisely the "large, regenerable, this machine
only" bucket.

**Portable mode.** Neither default is acceptable if the user wants the engine self-contained — on a
USB stick, or a checkout they can delete in one go. So both roots are overridable:

- `STRING_CONFIG_DIR` / `STRING_CACHE_DIR` override each explicitly.
- A `portable.txt` marker beside the executable puts **both** in `./config` and `./cache` next to the
  binary, and nothing is written outside the install directory. This is the conventional marker
  pattern and it makes portability a property of the install, not something re-specified per launch.

The existing `user_cache_dir()` Windows gap is a **pre-existing bug** worth fixing in the same pass:
today it falls through to the OS temp dir on Windows, which is wiped on reboot, so every launch cold
rebuilds shaders and meshlets.

**Not in scope:** cache eviction/size limits. Worth knowing it is unbounded and content-hash-keyed,
so it grows with every asset variant ever cooked — but a policy for that is its own decision, and
guessing at one here would be worse than leaving it visible.

### Are we reimplementing glTF? No — but the descriptor must stay smaller than one

A fair challenge (user, 2026-08-04), and the answer sets the format's boundary.

**Where glTF can express something, use glTF — do not restate it in the descriptor.** Cameras,
lights, transforms, materials and the scene graph are all things glTF already says, authored by the
artist in their DCC. Duplicating any of them in a sidecar means two sources of truth and a re-export
silently disagreeing with the descriptor.

**What glTF cannot express, and why the descriptor exists at all:**

- **Composing several glTF *files* into one world.** glTF has no external-file reference; sponza is
  three separate files (main + curtains + ivy) merged into a single draw set. No single glTF says
  that, and the artist may not have a combined export.
- **Scene kinds that have no glTF.** A Shadertoy scene (M4) and the procedural lookdev grid have no
  asset at all.
- **Engine-side settings** that are not scene content — which passes are enabled, exposure/GI
  defaults, stress knobs.

So the descriptor is a **manifest plus engine settings**, not a scene format. It should shrink over
time as more is read from the asset, not grow.

**Consequence: a single glTF needs no descriptor.** Treat any `*.gltf`/`*.glb` in the content root as
a scene in its own right — name from the filename, everything else from the asset. That is the
drop-it-in-and-see-it flow with zero authoring, and it makes descriptors the thing you add *only* to
compose multiple files or override something. Adopt this in M1; it makes the common case free.

### What the descriptor may encode: settings yes, pass topology no

DECIDED 2026-08-04, prompted by "USD and Blender encode renderer setup — is it wrong to do that?"

They do, and the line both draw is the useful one. USD's `UsdRender` schemas (RenderSettings,
RenderProduct, RenderVar, RenderPass) encode *what* to render — camera, resolution, AOVs, sampling,
which delegate — while Hydra's task graph, the actual execution topology, stays in the renderer and
out of the file. Blender stores per-scene render settings and a user-authored *compositor* node
graph, but EEVEE's internal passes are engine implementation, not `.blend` content.

The same split applies here:

- **Render SETTINGS in the descriptor: idiomatic, do it.** Exposure/EV100 defaults, GI on/off, sun
  angle + time of day, quality level. Directly analogous to UsdRenderSettings.
- **Pass TOPOLOGY in the descriptor: no.** Which passes exist varies per *build*, not per *content* —
  every content scene an artist makes wants the same pass set with different settings.

`scene = RenderPlan::Setup` (a pass set) is the current shape and the reason this needs saying. The
resolution: **passes are derived from `kind` + settings; the descriptor never enumerates passes.**
`kind` stays because it is genuinely content-level — geometry, procedural and shader scenes are
different *things*, not different pipeline tunings.

### Cooked output goes in the user's content folder (for now)

DECIDED 2026-08-04, explicitly revisitable. `.cooked` / `.ktx2` / `.cook_manifest` continue to be
written beside their source, which now means inside the user's chosen content folder.

Known trade, recorded so revisiting is informed: Unreal (DDC) and Unity (`Library/`) keep derived
data in a separate cache to leave the content tree clean, and ours will litter the artist's folder —
awkward if they version-control their content. Against that, sidecars keep a content folder
self-contained: hand it to someone else and the cooked data travels with it, so they skip the cook
entirely. The cache machinery (`user_cache_dir`, content-hash keyed) already exists if we switch.

### Writing cooked data back into the glTF: no

Also asked, and worth recording the reasoning so it is not revisited blind:

- **glTF has no meshlet concept.** Meshlet heaps, LOD chains and vertex windows would need a private
  extension — at which point it is not glTF interop any more, it is our format wearing a glTF costume.
- **Cooked data is derived and regenerable.** Writing it into the source inverts the dependency: the
  artist re-exports from Blender and silently destroys it, and now source and derived data disagree
  with no way to tell which is stale.
- The current arrangement — source authoritative, `.cooked`/`.ktx2` as disposable sidecars keyed by
  content hash — is what makes re-cooking safe and cheap. Keep it.

The one thing worth taking *from* the glTF that we currently drop is authored **cameras** (and later
`KHR_lights_punctual` lights): `CookedScene` carries geometry, materials and textures only, so the
cook needs to carry them through.

### The registry gains discovery; it does not gain a format

`SceneRegistry` stays what it is (name + description + `ConfigureFn`). A descriptor is turned into a
`ConfigureFn` **sandbox-side**, by a shared factory that closes over the parsed paths. The engine
does not learn what a `.scene` file is — asset formats and scene specifics stay app-side, per the
brief-README layering rule (precedent: KTX in the loader, `TransferBatch` format-agnostic).

`ui` and `lookdev` stay code-registered: neither has asset paths (one has no geometry, the other is a
procedural sphere grid with `lookdev=true` on the pass), so a descriptor would have nothing to say.
Every scene that *does* load assets — sponza included — goes through the data path.

## Milestones

Each ends with the standard gate: `nix build .#checks.{default,ui,cook}` green, `tools/gate.sh`
exterior AE=0 where assets are present, and no new validation output.

### M1 — Descriptor format + registry discovery + boots-with-no-assets

Add the nlohmann/json wrap. Add `user_config_dir()` (with the Windows branch, and fix
`user_cache_dir()`'s missing one) and persist the content root through it. Discover **both** bare
`*.gltf`/`*.glb` (a scene needing no descriptor) and `*.scene.json` (composition/overrides) under the
content root; register one entry each via a shared factory built on `make_geometry_setup`. **Delete the hardcoded sponza registration** and the `STRING_SCENE` fallback
to it; the fallback becomes `ui`.

**Gates, both required:**
- **Empty-asset boot** — with `sandbox/assets/sponza` moved aside (simulating the artist's fresh
  clone), the engine starts, the Scene menu lists only `ui`/`lookdev`, and nothing warns about a
  missing asset it was never asked to load. This is the requirement the brief exists for; test it by
  *removing* assets, not by reasoning that it should work.
- **Sponza-via-descriptor AE=0** vs the committed baseline, with a locally-written descriptor. If the
  data path renders identically to the literal it replaced, the abstraction is honest. Local-only —
  CI has no assets.

Plus: a malformed descriptor warns and is skipped, never aborts startup (same posture as the
unknown-`STRING_SCENE` fallback).

### M2 — Import: cook on demand

An import entry point that runs the geometry cook then `cook_scene_textures`, and writes a
descriptor with defaults (name from the filename, camera from a bounds-derived framing or a fixed
default — see open questions). Reuses the cook library the demo already links; no shelling out to
`string_cook`.

Import is a **blocking, logged** operation for this brief. Backgrounding it is a real want but it
needs progress reporting through the UI, and that is worth doing after the panel work rather than
inventing a one-off here.

**Gate:** importing an uncooked glTF produces `.cooked` + `.ktx2` siblings + a descriptor, and the
scene then appears in the menu and loads. Re-importing is a no-op (manifest staleness holds).

### M3 — Rescan + import in the Scene menu

"Rescan" re-reads the descriptor directory and rebuilds the registry entries. "Import glTF…" needs a
file path from somewhere — for this brief a cvar/console command is acceptable; a file picker is a
widget-set problem (brief 13), not this brief's.

Registry mutation must be safe against the active scene: rescan may not remove or replace the entry
currently loaded. Rescanning is a between-frames operation, same seam as `request()`.

**Gate:** NEEDS VISUAL VERIFY — new scene appears after rescan without a restart; switching to it
works; the active scene is unaffected by a rescan.

## Scope boundary

This brief gets you **view this asset as a scene**. It is explicitly NOT scene composition:

- no placing/transforming multiple assets in a scene
- no per-scene lighting authoring beyond what the descriptor carries
- no saving edits back to the descriptor
- no asset browser/thumbnails

That is the Tier-5 scene editor the 11–15 arc already scoped out to Phase B, and it stays out here.
The descriptor is a *manifest of what to load*, not a scene graph.

## M4 — Shadertoy scenes (shader prototyping)

A scene whose descriptor names a **shader** instead of models: one fullscreen pass, a standard input
block, and save-to-see-it. No code editing in-engine — the artist/programmer keeps their own editor
and the engine is the fast, forgiving preview half of the loop.

This belongs in brief 18 rather than a brief of its own because it *is* the data-driven scene idea
applied to a different payload. Discovery, the registry, the menu and switching all work unchanged.

### The REPL substrate already exists — do not rebuild it

`shader_program_registry` (brief 01) already does every hard part:

- recompiles asynchronously on the job pool when the watcher fires,
- **a failed compile leaves the running pipeline untouched** and records the diagnostic on the
  program, which the error overlay already surfaces,
- a success is staged and swapped at a frame boundary, with the old pipeline handed to the frame
  garbage collector so it dies after the GPU is done.

Keep-last-good plus a visible diagnostic *is* the "effective engine" the request is about: iteration
speed comes from never losing your session to a typo. M4 is a client of this, not an extension of it.

One deliberate exception to the startup contract: `create()` throws when a shader is broken at boot,
because a broken engine shader is a build error. A **user** shader broken at boot must not take the
engine down — it is the same not-yet-valid state as a broken save, so the scene loads with the error
overlay showing and recovers on the next save.

### This scene needs no assets

Which makes it a natural companion to the empty-state goal above: someone who clones the engine with
no assets at all can still write a shader and see it. Worth remembering when weighing M4's priority —
it is arguably the most useful thing an assetless clone can do.

### Decisions

- **Single pass in v1.** Shadertoy's Buffer A–D with feedback is a real want and a real scope jump
  (ping-pong targets, per-buffer descriptors, an execution order). Named here as deferred so v1 is
  not accidentally designed to exclude it.
- **`iChannel` inputs deferred with it** — they mostly exist to feed the buffer chain.
- **The descriptor grows a `kind`.** This settles open question 3 below in the opposite direction
  from the earlier recommendation: once shader scenes exist, the format genuinely needs a
  discriminator (`models` vs `shader`), and `lookdev` becoming `kind: procedural` stops being a
  special case and becomes the third value of an axis that already exists.

### DECIDED (user, 2026-08-04): the output BYPASSES post

Shadertoy shaders write **display-referred** colour and expect it on screen untouched. Our pipeline
is scene-referred HDR with EV100 exposure, auto-exposure, bloom and an ACES LUT — routing a ported
shader through that crushes its highlights and it stops matching its reference.

So a Shadertoy scene writes display-referred straight to the composite input: no exposure, no bloom,
no tonemap. What the shader writes is what you see.

Two consequences to implement deliberately rather than discover:

- **The UI overlay must still be correct.** UI is pre-divided by exposure elsewhere in the pipeline
  (brief 09); with no exposure applied here that division must not happen, or the HUD and error
  overlay come out wrong in exactly the scene where you most need to read the error.
- **The bypass is the whole path in v1.** A per-scene flag to route through the full chain (for
  prototyping actual engine effects in scene-referred HDR) was on the table and is NOT being built
  now — it is a plausible follow-up once the plain path works, not a v1 requirement.

## Dependency: the build must not require nix

This brief's whole point is handing the engine to someone else, so "it builds on my machine via the
flake" is not sufficient. Tracked separately (see `docs/off-nix-build.md`) because it is a build-system
task with its own verification, not scene work — but M3 is not shippable to an artist until it holds.

Note the nix/off-nix divergence is real but **narrow**: VMA and simdjson resolution, both already
handled with explicit fallbacks in `string-core/meson.build`. Adding JSON does not make it worse (it
comes with its own wrap) and does not make it better either — that cleanup is its own task, and
conflating the two would hide it.

## Open questions for the user

1. ~~**Initial camera pose for an imported asset**~~ **ANSWERED 2026-08-04, and it generalises.**
   Order: **the glTF's own camera if it has one**, else **derived from the cooked AABB**, else a fixed
   default for degenerate bounds. The authored camera is the view the artist chose — preferring it is
   the same principle as the section above: read what the asset already says. Requires the cook to
   carry cameras through (`CookedScene` drops them today). The descriptor's `camera` field becomes an
   *override*, not the source of truth.
2. ~~**Where descriptors live**~~ **ANSWERED 2026-08-04: a user-chosen folder** — see the decision
   above, plus the `user_config_dir()` prerequisite it pulls in.
3. **Does `lookdev` become a descriptor too?** ~~Recommend no~~ — **superseded by M4.** Once shader
   scenes exist the descriptor needs a `kind` discriminator anyway, so `kind: procedural` is no
   longer a special case bolted onto a path-list format. Revisit when M4 lands, not before.
4. ~~**M4: does Shadertoy output bypass post?**~~ **ANSWERED 2026-08-04: yes, bypass.** See the M4
   section; the route-through-post flag is not in v1.
