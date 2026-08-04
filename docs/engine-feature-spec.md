# String — MMORPG Client Engine: Status Review & Feature Spec

*2026-07-21, revised same day after the vision walkthrough. Scope: what String has
today, and what remains to be a complete engine for the MMORPG client. Companion to
the roadmap memory and `docs/*` plans.*

## Where the project is

The foundation is genuinely strong — this is past the "toy renderer" phase:

**GPU layer (solid).** Vulkan 1.3, `string::gpu` abstraction (device, presenter with
VRR + timeline-semaphore frame pacing, VMA allocator, bindless descriptor table,
persistent cross-frame `TransferBatch` with upload tickets, pipeline builder).
Programmable vertex pulling via device-address SSBOs (M2) — the mesh-shader
foundation is laid.

**Streaming/residency (solid, tested).** Generic `residency_manager` (policy-only,
unit-tested) over provider interfaces; KTX2 texture streaming with deferred
UASTC→BC7 transcode (Sponza load 15s → 495ms); geometry streaming with real
suballocation + VRAM reclaim, gated to zero cost when the scene fits. This is the
engine's differentiating asset for an open-world MMO.

**Rendering (good v1).** Metallic-roughness PBR (Cook-Torrance, derivative TBN),
directional shadow map with normal-offset bias + PCF, procedural sky + analytic IBL,
ACES tonemap composite, GPU frustum culling (`cull.comp`) + indirect draw.

**Text/UI (working v1).** SDF font atlas, layout engine with per-frame authored UI
(hover, focus, text input, modal game/UI input capture).

**Platform/core.** SDL3 (Windows path) + GLFW + raw Wayland backends, input mapping,
job system, logger, gtest suite, Nix flake build, Tracy vendored.

**Honest gaps in what exists:** `Scene` is nearly empty — the sandbox renders one
static flattened glTF; shadows are a single cascade fit to the whole scene AABB;
UI text is ASCII-only, no wrapping/kerning; Windows build untested; Tracy vendored
but not wired.

## Game-driven constraints (decided 2026-07-21 walkthrough)

These are facts about *the game* that bend the spec; each tier below is written
against them.

- **String becomes the client engine**; Godot client logic gets ported. No
  scripting layer, ever. Dev tooling (Tier 5) instead of an editor.
- **Gameplay is derisked elsewhere (decided 2026-07-21, reordered the plan).**
  Character gameplay and world generation are prototyped in the Godot repo with
  the real logic in **C++ via GDExtension** — porting is mechanical, not risky.
  The engine's true risk surface is **performance, robustness, and visual/UI
  satisfaction**. Therefore: **rendering-first ordering** — all rendering
  (Forward+, mesh shaders, VFX, post), UI maturation, and asset-style validation
  come *before* anything gameplay-touching (animation, scene, physics, procgen
  integration). The tiers below still describe what shipping requires; the
  ordering section describes the two-phase sequence.
- **Seamless open world** — full cell streaming, far-field LOD/impostors, and
  **origin shifting** (large-coordinate float precision; design in early).
- **Art: WildStar × PoE fusion** — stylized PBR with a pulled-back action camera.
  Consequences: current lighting model is sufficient (prefiltered IBL / TAA drop
  in priority); **combat-readability effects are first-class** — ground telegraphs,
  decals, PoE-density spell VFX, with an explicit overdraw budget; bloom is high
  priority; character close-up fidelity is moderate.
- **500+ animated characters** (mass battles) — animation LOD, shared joint
  palettes, instanced skinned draws, and crowd impostors are day-one design
  constraints, not optimizations.
- **Animation split: engine does sampling + blending only**; state machines are
  game code.
- **Full modular paperdoll** — characters assembled from per-slot skinned mesh
  parts sharing one skeleton, plus attachment props. **No morphs** (retrofittable
  as a separate vertex stream if a character creator ever needs them).
- **Client predicts the local player** (action combat) — needs client-side
  collision + character controller *early* (Jolt moves up in the ordering) and a
  fixed-tick sim loop from the scene milestone. All other entities interpolate
  server snapshots.
- **Scene data model: flat handle-based SoA tables**, not ECS. Mirrors server
  entities directly; enumerable for dev tooling.
- **Full dynamic time-of-day** (likely server-synced sun). The sky-derived-ambient
  architecture already in place is correct for this; constraint: any future
  prefiltered IBL must re-filter dynamically, never bake.
- **Procedural generation is an architectural pillar** — macro world baked,
  detail generated. **Hybrid by category**: gameplay-relevant detail (terrain
  shape, collision, resource nodes) is generated deterministically and shared
  with the server (same generation pipeline both sides); purely cosmetic detail
  (grass, scatter, debris) is generated client-side at runtime with no
  determinism requirement.
- **World authoring**: external tools for the baked macro layer; **in-engine
  placement/tuning tooling** (Tier 5, elevated) for set pieces and generation
  parameters.
- **Text: player-name-safe Unicode** — UI chrome stays English, but chat/names
  render arbitrary Unicode → dynamic glyph atlas; shaping stays simple.

---

## Feature spec

Ordered by what blocks an MMORPG client. Locked platform decisions from the
roadmap (Slang, mesh shaders, Forward+, SDL3, 3rd-party audio) are kept.

### Tier 1 — Can't ship a character without these

**1. Skeletal animation & GPU skinning.** The single biggest absent pillar. Spec:
- glTF skin/animation import (fastgltf already in tree); joint hierarchy, sampled
  keyframe curves (step/linear/cubic).
- CPU animation sampling + blending: N-way blend + layered masks (upper/lower
  body). Nothing more — state machines are game code.
- GPU skinning in the vertex-pull path: joint matrices in a per-frame SSBO;
  joints/weights as a second vertex stream (static geometry and the locked 44B
  `Vertex` unchanged).
- **Modular paperdoll from day one**: a character = per-slot mesh parts cooked
  against a shared skeleton + attachment sockets (bone-space transforms for
  weapons/mounts/props). Per-part materials ride the existing bindless table.
- **Animation LOD from day one** (500+ target): tiered sample rates by
  distance/importance, shared joint-palette buffers across instances, instanced
  skinned draws, impostor hand-off for the far tail (impostors themselves land
  with step 6's far-field work).

**2. A real Scene: many dynamic instances.** Today: one static model. Needed:
- Instance = mesh-part set + material refs + transform (+ skeleton ref). Stable
  handles in flat SoA tables; dynamic add/remove/move (players entering/leaving
  is the hot path). Instances carry a debug name + **server entity id** (dev
  tooling hook, and the natural key for snapshot application).
- Transform hierarchy (parenting for mounts/attachments), dirty-tracking.
- Per-instance GPU data (transforms SSBO), draw-list maintenance that doesn't
  rebuild the world when one goblin spawns.
- **Fixed-tick simulation loop + snapshot interpolation API** land here (not
  Tier 3): entities render interpolated between server snapshots; one predicted
  local entity slot reconciled against authoritative state.

**3. Client-side collision & character controller (Jolt).** Promoted from Tier 3
by the prediction decision — responsive action combat needs local simulation
before terrain even exists. Character controller vs. cooked static-mesh
collision, raycasts (picking, ground snap, line-of-sight), trigger volumes.
Jolt: proven, C++, permissive license. Collision artifacts join the cooker and —
per the procgen decision — the gameplay-relevant generated geometry must produce
identical collision on client and server.

**4. Forward+ clustered lighting** (locked). Point/spot lights as scene objects,
clustered light culling (compute), the existing Cook-Torrance BRDF carries over.
Target: hundreds of dynamic lights (spell density is PoE-like). Emissive included.

**5. Cascaded shadow maps.** 3–4 cascades, stabilized (texel snapping) — required
for the seamless world and the continuously moving sun. Streamed-geometry-aware
shadow draw list (currently static). Point/spot shadows later, budgeted.

### Tier 2 — Can't ship a *world* without these

**6. Procedural generation library.** New first-class item (was implicit). A
library defining the world-generation pipeline, consumed by client, server, and
cooker:
- **Deterministic core** for gameplay-relevant output (heightfield, collision,
  resource-node placement): same seed + macro data → identical results on client
  (Windows/Linux) and server. Determinism is a stated constraint: fixed-point or
  strictly controlled FP in this core; cross-platform determinism tests in CI.
- **Non-deterministic cosmetic layer** (grass, scatter, debris) generated
  client-side at runtime, free to use whatever is fast.
- Where it runs: gameplay-relevant detail at cook-time or server-authoritative
  generation; cosmetic detail at chunk stream-in on the client (generation as a
  residency *provider* — the manager doesn't care whether a provider reads disk
  or runs a generator).
- Cross-repo contract with the server (shared library or shared artifact format —
  decide with the server repo).

**7. Terrain + open-world streaming.** Seamless world, hardest version:
- Chunked heightmap terrain fed by the baked macro layer + generated detail;
  material splatting (reuse BC7 pipeline); holes for caves/entrances. Chunk
  representation chosen after mesh shaders land (meshlet terrain is a natural fit).
- World cells streamed by camera position (instances + assets + generated chunks)
  on the existing `residency_manager`.
- **Origin shifting** for large coordinates.
- Far-field: LOD/impostors for distant chunks *and* distant crowds (shared
  impostor system with #1's animation LOD tail).

**8. Mesh shaders / meshlets + LOD** (locked, founded by M2+M5). meshoptimizer →
meshlets → task/mesh draw path replacing `cull.comp` indirect; per-meshlet cone/
occlusion culling; discrete LOD per instance. Enables terrain chunks and 500+
crowd geometry throughput.

**9. VFX: particles, telegraphs, decals.** Promoted to a core pillar by the
WildStar × PoE direction — this is combat readability, not garnish:
- GPU-simulated particles (compute update, indirect draw), soft billboards,
  additive/alpha modes, trails/ribbons, mesh emitters.
- **Ground telegraphs as a first-class system**: projected shapes (circles,
  cones, lines, donuts) with sharp edges, fill/warning states, batched — driven
  directly by server combat data. Ground-projected decals (targeting circles,
  impact marks) share the machinery.
- Data-driven effect descriptions (hot-reloadable files, not code).
- **Explicit overdraw budget** and measurement (PoE-density fights are a
  transparency stress test; profile this from the first effect).

**10. Transparency & material variety.** Sorted alpha-blend pass, alpha-tested
cutout in depth/shadow passes, two-sided materials (fixes the Sponza black-cracks
class of bug), emissive; water eventually.

**11. Post-processing chain.** Reprioritized for stylized art: **bloom first**
(sells the style + spell glow), auto-exposure, SSAO (modest), outline/rim support
if the art direction wants it. TAA/upscaling and prefiltered IBL *deprioritized* —
revisit when content exists; dynamic TOD means IBL, if it ever lands, must
re-filter continuously.

### Tier 3 — Can't ship a *game* without these

**12. Audio** (locked: 3rd-party). Recommendation: **miniaudio** (single-header,
matches dependency posture). Thin engine surface: sound handles, 3D positional
emitters on scene instances, listener = camera, streaming music, bus volumes.

**13. Asset pipeline / cooker.** DONE 2026-08-02 — `tools/cook_textures.sh` is now a library stage in string-asset-tools, run by `string_cook` alongside the geometry bake. Remaining scope for one cooker:
- glTF → engine-native pack: meshlets (meshopt), BC7 KTX2, skeletons/anims,
  **paperdoll parts cooked against shared skeletons**, collision — cooked to the
  exact GPU/disk layout the streamers read.
- Runs the deterministic procgen core for cook-time-generated world artifacts;
  emits the server-consumable heightfield/collision artifact (cross-repo
  contract with the server).
- Content-hash incremental cooking; manifest with dependencies.
- **Patch-friendly pack format** (content-addressed chunks → CDN delta updates) —
  design in now; procgen helps here (parameters patch smaller than data).

**14. Game-facing runtime services.** The layer that makes it an engine:
- Virtual filesystem (loose files in dev / packs in release), with **stable asset
  ids + id→path indirection** (dev-tooling live-swap hook).
- Config/CVar system + persistent settings (resolution, quality tiers, keybinds —
  input_map exists, needs serialization).
- Crash handling: minidump/stacktrace + log flush (wanted by the first playtest).
- (Fixed-tick loop moved up to #2.)

### Tier 4 — Quality, workflow, ship-readiness

**15. Slang + shader hot-reload** (locked; mtime polling on job_system). Do this
*first* in practice — it multiplies velocity on everything above. Generalize the
watcher beyond shaders from day one (UI layout, effects, tuning, procgen params).

**16. UI to game-grade** (pulled forward — a Phase A pillar, not late polish).
Two explicit goals set 2026-07-21: a **powerful but simple authoring abstraction**
(the current authored-callback + layout-tree model is the right shape — mature it,
don't replace it) and a **satisfying feel**. Spec:
- *Abstraction robustness*: full **dynamic glyph atlas** for player-name-safe
  Unicode (chat/names render anything; chrome stays English; shaping stays
  simple — no HarfBuzz unless localization grows), kerning + word wrap,
  scrolling + clipping, textured/9-slice panels (bindless atlas — machinery
  exists), draggable/resizable windows, item-grid/tooltip/drag-drop patterns,
  gamepad-navigable focus, and a small set of composable widgets built *on* the
  callback model rather than a retained widget tree.
- *Feel*: **animation as a first-class layout property** — eased transitions on
  position/size/color/opacity (hover states that glide, panels that slide/fade,
  press feedback), spring/ease curve library, per-element transition declarations
  in the authoring API so motion is one line, not hand-rolled per widget. Input
  responsiveness measured (click-to-visual-response latency), hooks for UI sound
  cues (wired when audio lands). This is where "satisfying" lives; treat it as a
  spec requirement, not garnish.

**17. Debug & profiling toolkit.** Wire the vendored Tracy (CPU+GPU zones);
in-game debug HUD on the existing UI (frame times, residency/streaming stats,
draw counts, **overdraw view** for VFX budgeting); debug draw API (lines/AABBs/
skeletons — needed immediately by #1 and #3); validation-layer CI run;
**cross-platform procgen determinism tests**.

**18. Windows ship path.** SDL3 backend exists; make the Meson/Nix→Windows build
real (MSVC or clang-cl), CI matrix, Slang toolchain parity. Determinism of the
procgen core across the compiler split is part of this. An MMO with no Windows
build has no players.

**19. Robustness.** Device-lost recovery (or clean error + relaunch), swapchain
edge cases, out-of-VRAM degradation via the residency budget (machinery exists —
needs quality-tier policy), GPU crash breadcrumbs.

### Tier 5 — Dev tooling (elevated from pure stretch by the authoring decision)

Not a Godot/Unity editor — no scripting, no node graphs. Live development
instrumentation for a server-authoritative client, plus the **in-engine world
authoring** the procgen workflow needs. Reads through the same handles/tables the
game uses — no parallel editor data model; that's the guardrail against becoming
the monolithic editor the README rejects.

- **Scene inspector.** Read-only live view of client world state: entity list
  (server id ↔ instance handle), transforms, mesh/material/skeleton/anim state,
  residency status, lights; selected entity gets AABB/skeleton debug-draw
  highlight. *Hook (Tier 1): debug names + server ids + enumerable tables.*
- **In-engine placement & procgen tuning** (the elevated part). Fly-cam
  place/move/save for set pieces (position/rotation/asset-id lists to data
  files) and **live procgen parameter tuning** (edit params → regenerate the
  streamed-in chunks in view). External tools still own the baked macro layer.
- **Live asset swap while connected.** Point an asset id at a different cooked
  file against the live server: evict + re-want through the residency manager.
  *Hook (Tier 3): id→path indirection.*
- **Shader/tuning hot-swap.** The Slang watcher (#15) generalized to effects,
  tuning files, CVars; small console panel in the debug HUD.

### Deliberately out of scope (per README principles)

Monolithic editor and general authoring tools (Tier 5 is the bounded exception),
custom scripting language, mobile/console/Apple, GI (Lumen-class), ray tracing,
nanite-style virtualized geometry (meshlet LOD is the 80%), networking stack
(stays game-side; engine provides the fixed-tick + snapshot-interpolation scene
API), TAA/upscaling until content proves the need.

---

## Suggested ordering (revised 2026-07-21: rendering-first)

Gameplay is derisked in the Godot/GDExtension repo, so the engine attacks its own
risk surface first: **Phase A proves performance, robustness, and visual/UI
satisfaction with zero gameplay coupling. Phase B is everything gameplay-touching,
started only when Phase A is satisfying.** Prototyping in the other repo continues
in parallel during Phase A.

### Phase A — rendering, UI, feel

(Reordered 2026-07-22 after step 3 closed: pipeline solidification, UI, debug
tooling, and PBR now precede VFX/post — briefs 01–03 proved tooling leverage,
and VFX/post want the HDR/PBR foundation under them.)

1. **Slang + hot reload** (#15) — DONE. The iteration loop for everything in
   Phase A.
2. **Forward+ clustered lighting** (#4) + **CSM** (#5) — DONE. Dynamic-TOD
   outdoor lighting with hundreds of lights.
3. **Mesh shaders / meshlets + LOD** (#8) — DONE. The committed (and now sole)
   geometry pipeline: GPU draw-cull → indirect-count mesh-task draws → task
   frustum/cone/HiZ → mesh; 500+-crowd geometry throughput proven with
   instanced static stand-ins (skinning attaches in Phase B).
4. **Renderer solidification** — **transparency/cutout/two-sided** (#10, fixes
   the Sponza cracks class) + brief-03 parked debt (index-heap dead weight,
   per-cascade shadow draw cull). Harden the single path before building on it.
5. **UI maturation + feel** (#16) — pulled forward: the abstraction hardening
   and the motion/easing system; mock game screens as the acceptance test. The
   tooling in step 6 builds its panels on this.
6. **Debug & profiling toolkit** (#17) — pulled forward from the checkpoint:
   CVars + console, per-pass GPU timestamps + Tracy, debug draw API, scene/draw
   inspector, first-class capture/diff. Every later step is built and verified
   through these.
7. **Proper PBR** — dynamic sky IBL (prefiltered specular + SH diffuse,
   TOD-driven, no bakes), physical-ish light units, EV100 exposure model.
   Completes #4's lighting into a real PBR pipeline before content is authored
   against it.
8. **VFX: particles + telegraphs + decals** (#9) — combat readability pillar,
   stress the overdraw budget with PoE-density synthetic fights; effects
   authored in HDR units from day one.
9. **Post-processing** (#11) — bloom, auto-exposure, SSAO; the stylized look
   locks in here.
10. **Asset style validation** — a dedicated checkpoint, not a feature: lookdev
    on Sponza/Bistro + a small stylized probe; iterate materials/lighting/post
    until the WildStar × PoE look is *right*; outline/rim + tonemapper
    decisions land here.
11. **Robustness + Windows** (#18, #19) — ongoing through Phase A, gated hard
    before Phase B: validation-clean, resize/alt-tab solid, Windows build in CI,
    frame-time budgets held in the synthetic stress scenes.

### Phase B — gameplay-touching (starts only when Phase A satisfies)

9. **Skeletal animation + paperdoll** (#1) — onto the proven meshlet path;
   animation LOD for 500+ designed in from the start.
10. **Scene/instances + fixed tick + interpolation** (#2) — the port target for
    the GDExtension C++ logic.
11. **Jolt: collision + character controller** (#3) — local-player prediction.
12. **Procgen library** (#6) + **terrain + world streaming** (#7) — integrate
    the world-generation work prototyped in the other repo; coordinate the
    server contract here.
13. **Audio** (#12), **cooker** (#13), **runtime services** (#14) — cooker
    likely earlier if Phase A's asset checkpoint demands it.
14. **Dev tooling** (Tier 5) — hooks baked in throughout; placement/procgen
    tuning tools land with step 12.

Phase A's exit criterion is subjective on purpose: the sandbox scenes look like
*your game*, the UI feels satisfying to poke at, and the frame budget holds under
synthetic crowd/VFX stress — before a single line of gameplay is ported.
