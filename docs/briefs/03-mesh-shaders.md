# Brief 03 — Meshlet pipeline, HiZ occlusion, LOD (detailed spec)

Status: CLOSED / COMPLETE (2026-07-21). The task/mesh meshlet path is THE path: GPU-driven single
indirect-count draws (03b) with two-pass HiZ, cone/frustum cull, and discrete LOD. The old
vertex-input pipeline (cull.comp + vkCmdDrawIndexedIndirect + 3d_shader/shadow.slang + CullDraw +
the M/STRING_PATH/CPUDISPATCH toggles) was deleted in 03c — verified deletion-neutral by exact
(AE == 0) headless captures at both standard cameras with HiZ on and off, identical [mesh-cull]
stats, clean validation, and working hot-reload.

Status (historical): PARITY + STABILITY FIXES LANDED (2026-07-21, agent verify via headless capture + AE diff).
Root causes found & fixed: (a) the "darker" meshlet path was NOT a shading/shadow bug — it was
over-aggressive discrete LOD. meshopt's quadric result_error under-reports perceptual (silhouette/
normal) deviation ~30x, and it was accumulated per-step (relative to the shrinking previous LOD),
understating cumulative error, so coarse LODs were picked far too close. Fix: simplify each LOD from
LOD0 (true cumulative error) + retune the SSE threshold to 0.02px. Meshlet geometry+shading is now
pixel-parity with the vertex path (LOD off: <0.1%; LOD on: <0.2% at 3 cameras). (b) HiZ two-pass
visibility bitfield DELETED -> single main pass tests every meshlet vs this frame's complete-prepass
pyramid: drawn counts now deterministic (identical across runs) and exterior HiZ on-vs-off = 0%.
Interior HiZ over-cull cut 3.6%->1.3% (conservative rect grid-sampling + bounds inflation); residual
is thin ceiling-beam slivers past the curtain silhouette (inherent sphere-HiZ limit, > the 0.2%
target). (c) occlusion-reject debug view (3) now force-draws HiZ-rejected meshlets tinted red.
(d) shader disk cache now folds the import closure into the key (imported-module edits invalidate).
OLD PATH REMAINS DEFAULT. Punch list + tooling notes in the roadmap memory.

## Goal

Replace the vertex-input + `cull.comp` per-draw indirect path with the committed
**task/mesh shader meshlet pipeline** (`VK_EXT_mesh_shader`), including two-pass
**HiZ occlusion culling in v1** and discrete per-draw LOD. Prove 500+-crowd
*geometry throughput* with instanced static stand-ins — skinning attaches in
Phase B through a documented seam. The old path is **deleted** (locked: no
fallback, ≥5-year hardware; target GPU RADV RDNA3 supports EXT_mesh_shader).

## Decisions (locked with user)

- Two-pass HiZ occlusion in v1 (user chose the full landing).
- Per-meshlet frustum + backface-cone culling in the task shader.
- Discrete LOD per draw (meshopt simplify chain). No continuous/nanite LOD.
- Mandatory debug aids land WITH the pipeline (details below).
- Skinning attach seam documented, not implemented.
- Meshletize at load + content-hash disk cache (real cooker is Phase B).

## Prerequisites / plumbing (milestone 0)

- Device: enable `VK_EXT_mesh_shader` (feature struct: `taskShader` +
  `meshShader`), extension in device creation, and add it to the suitability
  check. meshoptimizer: new dependency (meson wrap + nix, like ktx/SDL3 —
  NOT vendored; it's actively maintained).
- `pipeline_builder`: mesh pipelines have NO vertex-input/input-assembly state;
  stages are TASK+MESH+FRAG. Add `add_task_shader_spirv` / `add_mesh_shader_spirv`
  and a `build_mesh_pipeline` (or a flag) that omits the VI states.
- `shader_compiler` / registry: map Slang stages — Slang uses HLSL naming:
  `[shader("amplification")]` = task, `[shader("mesh")]` = mesh
  (SLANG_STAGE_AMPLIFICATION/SLANG_STAGE_MESH → VK task/mesh bits). All new
  pipelines go through the hot-reload registry.

## Data model (the meshopt reference flow — do not improvise)

Per source draw, built at load with meshoptimizer and disk-cached (content hash
of the index/vertex ranges, same pattern as the shader cache):

- `meshopt_buildMeshlets` with **64 max vertices / 124 max triangles**,
  `cone_weight = 0.5` (constants grouped for tuning). Then
  `meshopt_computeMeshletBounds` per meshlet.
- GPU buffers (all device-address SSBOs, matching the existing vertex-pulling
  style):
  - `Meshlet { uint vertex_offset; uint triangle_offset; uint8 vertex_count;
    uint8 triangle_count; ...; float3 center; float radius; int8x3 cone_axis;
    int8 cone_cutoff; }` (quantized cone per meshopt docs; pad to 16B alignment).
  - meshlet-vertices: uint remap into the shared vertex heap (global indices —
    keeps the existing HeapSuballocator/global-index scheme intact).
  - meshlet-triangles: packed 8-bit local indices (u32-packed).
  - Per-draw table: `DrawInfo { model matrix (or transform index), material
    slots, meshlet_offset/count per LOD level, lod_count, resident flag,
    vertex heap base }` — replaces `CullDraw`.
- **LOD chain**: `meshopt_simplify` per draw, up to 4 levels, each targeting
  ~50% of the previous index count with a bounded error; store per-LOD meshlet
  ranges + the simplification error in DrawInfo. Meshletize every LOD.

## Draw path (per frame, both passes share this shape)

1. **Task dispatch**: one workgroup per N meshlets (32) of each visible draw.
   Task shader tests each meshlet: frustum (6 planes vs sphere), cone
   (backface: `dot(cone_axis, normalize(center - eye)) >= cone_cutoff`
   quantized form), HiZ (pass 2 only / last-frame pyramid in pass 1 — see
   below), and emits surviving meshlet indices via payload to mesh workgroups.
- 2. **Mesh shader**: reads meshlet, fetches vertices via the remap from the
   vertex heap (buffer_reference, brief-02 layout incl. tangents), applies the
   draw transform, emits ≤64 verts / ≤124 tris. Fragment stage: the SAME lit
   fragment module as brief 02's Forward+ (import it — one lighting
   implementation, two geometry front-ends during bring-up, one after deletion).
3. **LOD select**: per draw, before task dispatch (small compute or in the task
   shader's first lane): screen-space-error metric — project the draw's bounds;
   pick the coarsest LOD whose stored simplification error projects below a
   threshold (~1px at 1080p; constant, tunable). Standard, camera-driven,
   procgen-friendly (no per-asset tuning).

## Two-pass HiZ (standard scheme — keep to it)

Persistent per-meshlet visibility bitfield buffer (indexed by global meshlet id).

- **Pass 1**: draw meshlets marked visible last frame (task shader reads the
  bit; frustum+cone still applied). This produces a near-complete depth buffer.
- **Pyramid build**: compute downsample of the depth buffer into an R32F mip
  chain. Reverse-Z: reduce with **min** (farthest). Power-of-two conservative
  sizing; sampler clamps.
- **Pass 2**: task shader tests every meshlet NOT drawn in pass 1 against the
  pyramid (project sphere → screen rect → conservative mip → compare vs
  farthest depth); draws the newly-visible, writes the updated visibility bit.
  Meshlets drawn in pass 1 also re-test to update their bit for next frame.
- First frame / teleports: visibility buffer zeroed → pass 1 draws nothing,
  pass 2 draws everything (correct, one slow frame; fine).

## Shadow path

CSM cascades (from brief 02) render through a depth-only task/mesh pipeline:
frustum cull per cascade + cone cull, **no HiZ** (no pyramid for light views in
v1). This replaces brief 02's interim indirect shadow draw list. Alpha-cutout
support is NOT needed here yet (no cutout materials until brief 04) — note it.

## Streaming seam (do not break M5)

`GeometryStreamer` gates draws today by writing `CullDraw.index_count` when a
range finishes uploading. `cull.comp` and `CullDraw` are deleted; the equivalent
gate becomes the **`resident` flag in DrawInfo** (host-visible write, same
pattern). Meshlet/LOD metadata buffers are built at load and always resident
(small — metadata only); only the vertex/index heaps stream. Suballocator, heap
sizing, and eviction logic are untouched; eviction clears the resident flag
(today it zeroes cull fields — same one-line seam).

## Skinning seam (design constraint, document in code)

Mesh shader vertex fetch goes through ONE function
(`load_vertex(draw, index) -> Vertex`) so Phase B can point it at a skinned
vertex stream per instance. Meshlet bounds/cones assume static positions —
document that skinned draws will need bounds inflation + cone-cull bypass
(a per-draw flag reserved in DrawInfo now).

## Debug aids (mandatory, land WITH milestone 3, not after)

- Freeze-cull camera: cull from a frozen camera while the view camera flies
  free (toggle; reuse the existing input/UI author for the keybind).
- Meshlet-ID color view; LOD-level color view; occlusion pass/fail view
  (draw HiZ-culled meshlets in red wireframe or flat overlay).
- Stats: GPU-written counters (meshlets total → after frustum → after cone →
  after HiZ; draws per LOD) read back and shown in the UI overlay + log line.

## Milestones (each: flake check green, demo builds, report)

0. Plumbing: extension/features, pipeline_builder mesh support, Slang
   task/mesh stage support through compiler+registry (prove with a trivial
   mesh-shader triangle behind a debug toggle).
1. meshopt dependency + meshlet/LOD build at load + disk cache (report build
   times + cache hit timings for Sponza).
2. Task/mesh pipeline drawing ALL meshlets (no culling), old path still
   active behind a toggle → **pixel-parity checkpoint on Sponza (user
   verifies) → delete cull.comp, indirect path, CullDraw, and the toggle.**
3. Frustum + cone culling in task shader + ALL debug aids + stats.
4. Two-pass HiZ + pyramid build + occlusion debug view. (Watch: reverse-Z
   min-reduce, conservative mip selection, first-frame empty visibility.)
5. LOD chain + screen-space-error selection + LOD color view.
6. Shadow cascades on the meshlet path (delete interim shadow draw list) +
   **crowd stress scene**: thousands of instanced stand-in meshes ≈ 500-player
   battle geometry (duplicate draws with varied transforms is fine); report
   culling-stats and frame timings from logs.

## Acceptance

- Milestone-2 parity confirmed by user before any deletion; validation clean
  throughout; streaming still works (stream-in/evict flips DrawInfo.resident).
- Freeze-cam shows correct culling: nothing visible ever missing; stats show
  meaningful frustum/cone/HiZ reduction in interior views (Sponza atrium from
  inside should HiZ-cull heavily).
- Hot reload works for task/mesh shaders (edit → save → swap).
- Crowd stress holds frame budget with numbers reported.
- NEEDS VISUAL VERIFY at milestones 0 (triangle), 2 (parity), 3–6 (views,
  stability, stress) — agent reports what to look for, never claims visuals.

---

## Brief 03b — Full GPU-driven draw generation (single indirect draw call)

Status: LANDED (2026-07-21, agent verify via headless capture + AE diff). The per-draw CPU dispatch
loops (main / HiZ prepass / 3 shadow cascades) are replaced by a GPU draw-cull compute
(meshlet_draw_cull.slang) writing one indirect command + a {draw_index, lod} record per draw, drawn
with a single vkCmdDrawMeshTasksIndirectCountEXT per pass; the task shaders read their record via
SV_DrawIndex. LOD select moved to the compute (verbatim port of the CPU screen-space-error math).
KEY FIX: the work list is written at a STABLE slot (== draw index; skipped draws get groupCountX=0),
NOT atomically compacted — an atomic-compacted list's submission order raced frame-to-frame and
produced non-deterministic z-fighting on coplanar interior meshlets (13% run-to-run). Stable slots
keep the draw order identical to the old sequential loop -> pixel-deterministic. Verified: interior
parity 0.096% / exterior 0.016% (meshlet-vs-old, unchanged from pre-03b), HiZ on-vs-off 1.27%
interior (unchanged), meshlet cull stats BIT-IDENTICAL at both cameras (compute does NO draw-level
frustum cull, so meshlets_total is preserved), determinism 0-diff x3, base frametime not worse
(interior 3.61->3.58 ms). Crowd (14580 draws) A/B interior: CPU-dispatch main pass 42.2 ms vs
GPU-driven 39.0 ms (~8% on the main pass alone; the full old path also CPU-looped prepass+3 cascades).
STRING_CROWD=1 headless env + STRING_CPUDISPATCH=1 A/B lever added; STRING_DRAW_MIN/MAX removed.
OLD PATH + M TOGGLE STILL PRESENT (deleted in a separate pass after user A/B sign-off).

### Design (three-level GPU pipeline)

1. **Draw-cull compute** (`meshlet_draw_cull.slang`, cull.comp's successor): one thread per
   draw over the DrawInfo table (resident flag is the streaming gate, already GPU-visible).
   Per draw: sphere/AABB frustum test (draw bounds already in DrawInfo) + **GPU LOD select**
   (port the CPU screen-space-error math verbatim; DrawInfo.lods[].error has the true-vs-LOD0
   errors from 03's fix) → atomically append to a work list:
   - `commands[]`: `VkDrawMeshTasksIndirectCommandEXT { ceil(meshlet_count/32), 1, 1 }`
   - `records[]` (parallel array): `{ draw_index, lod }`
   - `count` buffer for the indirect count.
2. **One `vkCmdDrawMeshTasksIndirectCountEXT`** for the main pass. The task shader reads its
   record via the draw index builtin (Slang: `SV_DrawIndex` / DrawIndex — shaderDrawParameters
   is already enabled) instead of push-constant draw_index/lod (remove those push fields;
   re-verify %Push_std430 offsets).
3. Task shader unchanged otherwise (meshlet frustum/cone/HiZ), mesh/fragment unchanged.

### Consumers of the work list

- **Main pass and the HiZ depth prepass share ONE camera work list** (the prepass draws the
  same pre-HiZ survivor set at the same LODs — that is exactly what the list holds).
- **Shadow cascades**: same cull compute dispatched per cascade with the light VP (no LOD
  change needed: reuse camera-selected LOD records but re-test bounds vs the light frustum;
  3 small lists). CPU records one indirect-count call per cascade.

### Plumbing

- Enable `drawIndirectCount` (Vulkan 1.2 feature) in BOTH device chains (query ~line 73 +
  enable ~line 217 of device.cpp); buffers need `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`.
- Barrier: cull-compute writes → `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT` /
  `VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT` (+ task-stage storage read for records).
- Stats: the LOD histogram moves into the draw-cull compute; meshlet-level stats stay.
- Delete: the CPU record loops (main/prepass/cascades), CPU `draw_lod_` computation and
  array, `STRING_DRAW_MIN/MAX` bisection env (now meaningless — note removal), per-draw
  push fields. KEEP `debug_draw_min_`-style bisection IF trivially re-expressible in the
  cull compute via push (optional, low priority).
- Crowd: add `STRING_CROWD=1` env override triggering the K-toggle code path at first
  update, so the crowd scene is headlessly benchmarkable.

### Acceptance (numeric, same methodology as the fix round)

- Pixel parity: meshlet-vs-old cropped diffs at the two standard cameras unchanged from
  current (≤0.16% interior / ≤0.02% exterior, lights off, HiZ off); HiZ on-vs-off residual
  not worse than current 1.27% interior / 0% exterior.
- Culling stats: frustum/cone/hiz meshlet counts IDENTICAL to the current implementation at
  both cameras (same tests, same inputs); LOD histogram may differ only by float-rounding
  (a few draws at boundaries — justify any delta).
- Determinism: identical stats across 3 runs.
- Frame time: `[frametime]` A/B at both cameras not worse than current meshlet path, and
  **crowd (STRING_CROWD=1) must show the CPU-dispatch win** — report all numbers.
- `nix flake check` + `.#demo` green; smoke runs validation-clean; no commits (user commits).

---

## Brief 03c — Old-path deletion (final close-out; user signed off 2026-07-21)

The meshlet path passed all gates (parity, HiZ motion stability, full freeze-cull, GPU-driven
draws). Delete the old vertex-input pipeline entirely; the meshlet path becomes THE path.

DELETE:
- `sandbox/shaders/cull.comp`, `3d_shader.slang`, `shadow.slang` (+ their meson.build entries;
  cull.comp is the last glslang-compiled scene shader — remove its glslang build rule; keep
  grid_2d GLSL files untouched, they are out of scope).
- GeometryPass: `CullDraw`/`CullPush`/`DrawData` structs + `cull_draw_buffer_`/`cull_draw_mapped_`/
  `culled_indirect_buffers_`/their creation, destruction, barriers; the cull.comp pipeline program;
  the 3d_shader lit program + shadow.slang program + old shadow indirect list; the entire
  `!use_meshlets_` record branches (old vkCmdDrawIndexedIndirect draw, old cull dispatch, old
  shadow rendering); the DrawData bindless slot fill.
- `use_meshlets_` flag + M keybind + `STRING_PATH` env + "path:" overlay line (now always meshlet);
  `STRING_CPUDISPATCH` lever; `meshlets_active` overlay field.
- GeometryStreamer residency callback: drop the cull_draw_mapped_ writes; keep DrawInfo.resident +
  vertex_offset (now the ONLY gate). Callback signature may simplify if trivial.
- KEEP: vertex/index heaps + streamer internals (index heap is now GPU-dead weight — note as a
  follow-up comment, do NOT refactor the streamer), aabb_in_frustum CPU residency feedback,
  freeze/C/O/V/G/K controls, lighting.slang/sky.slang (meshlet fragment + sky use them).

ACCEPTANCE (deletion changes NOTHING visually):
- BEFORE deleting: capture meshlet-path references at both standard cameras (lights off, HiZ on
  AND off). AFTER: identical captures — `magick compare -metric AE` == 0 px (not fuzz — exact).
- `[mesh-cull]` stats identical before/after. `nix flake check` + `.#demo` green; 30s smoke
  validation-clean; hot reload still works (edit meshlet_mesh.slang, see it swap).
- grep proves no dangling refs: `use_meshlets_|CullDraw|cull_draw|culled_indirect|cull.comp|
  3d_shader|shadow.slang|CPUDISPATCH` → zero hits in sandbox/ + string-engine/ source.
