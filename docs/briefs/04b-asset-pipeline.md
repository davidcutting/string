# Brief 04b — Asset pipeline v1: bake library + cooked scene format + chunking

Status: M1-M4 DONE (2026-07-22). M1: bake library + cooked format + gtests (round-trip,
determinism, chunking, window-compactness) green via `checks.cook`. M2: engine loads cooked scenes
(warm ~1.4s geometry, no fastgltf/MikkTSpace/meshopt; cold in-process cook ~24s writes the file);
parity AE=0 interior+exterior; `~/.cache/string/meshlets` deleted (shader cache kept). M3: `.#cook`
flake app + manifest incremental re-cook (size+mtime fast path + cooked-header format-version
check); all three packs cooked. M4: cooked format v2 (vertex stream repacked grouped-per-draw — v1
chunk windows spanned the whole parent draw and blew up the streamer heap 17x); chunking ON,
kChunkMaxMeshlets=1024 (tuned: 256 gave 2852 draws whose per-draw indirect-record overhead cost
more than tight-bounds culling saved; 1024 = 954 draws). Frame times (STRING_LIGHTS=0, 780M):
interior 9.8->10.9 ms, exterior 8.6->9.3 ms, crowd 93.9->127 ms vs chunking-off — the acceptance's
"back toward ~5 ms" is NOT met: meshlet-level frustum/cone/HiZ culling (brief 03) already handles
giant draws (post-HiZ survivors ~unchanged), so the interior cost is genuinely-visible ivy raster,
and per-draw record overhead (~2-5 us/draw/frame across prepass+main+3 cascades, confirmed
independently by crowd scaling) makes finer chunking a net loss. Recovering it needs a global
meshlet list / multi-draw batching (follow-up in the brief-03 pipeline, not the bake). Draw order
deterministic: run-to-run captures byte-identical (no z-fighting flicker) — independently confirmed
AE=0 chunked-vs-chunked on interior+exterior+crowd. STRING_TRANSP_TEST fixed: the M4 quad-injection
block double-rebased the material index (set absolute `base_mat+q` then re-added `base_mat` on merge),
hitting a std::vector OOB abort at load with the test on (fired both chunked and unchunked, so not a
chunking bug); now sets a file-local index and rebases once — verified rendering 5 sorted blended
quads. Added `STRING_CHUNK` env override in GeometryPass (selects the `.c<budget>.cooked` variant;
`STRING_CHUNK=0` = unchunked) for reproducible A/B without recompiling. Validation clean (only the
pre-existing dzn ICD -9 skip). NEEDS VISUAL VERIFY: post-chunking flythrough (ivy/curtains, no popping
or flicker); the M4 capture pairs at /tmp/m4caps/*.png (or re-capture with tools/capture.sh).

CHUNKING DECISION RESOLVED (brief 04c M4, 2026-07-22): chunking is FINALIZED **OFF** (r.chunk.budget
default 0 = unchunked cooked variant). The "global meshlet list" follow-up this brief pointed to
(04c) landed as resolution (b) — order-stable scan-COMPACTED per-draw command boundaries — which the
RADV/RDNA3 determinism gate forced (a single draw-count-independent dispatch is non-deterministic on
that driver at coplanar surfaces). Because the deterministic design keeps per-draw command boundaries,
chunking's per-draw fan-out is still NOT free: matched A/B shows interior chunked ~+2.5 ms GPU
(main-draw +1.13, shadow-cascades +0.74, hiz-build +0.62) vs unchunked, with no quality/determinism
benefit. So the 04b acceptance's "chunking pays off once fan-out is gone" does not hold — fan-out is
NOT gone (it can't be, without losing determinism). Default OFF. See 04c M4 for the full table.

## Goal

Promote the accidental half-pipeline (KTX2 texture cook + a runtime meshlet
cache pretending to be a cook) into a real one: a **bake library** both an
offline CLI and the engine link, a **cooked scene format** the engine consumes
directly, and **spatial chunking** of oversized draws (the ivy problem). After
this brief the engine never runs fastgltf/MikkTSpace/meshopt at load when a
cooked scene is present, loads are deterministic on any machine, and the
per-user cache-warming disappears.

## Context / motivation (2026-07-22)

- The demo scene is now 3 merged glTFs (main + curtains + ivy): 450 draws,
  6.4M verts, 240k meshlets. Cold load re-runs parse + flatten + MikkTSpace +
  a **21-second meshletize**, memoized per-user in `~/.cache/string/meshlets`.
- The ivy pack packs 3.27M verts / 119k meshlets into **2 draws** whose bounds
  span the whole building — draw-level frustum cull, per-cascade shadow cull,
  and per-draw LOD are all defeated; interior frame time went 3.9 → 9.9 ms.
- The procgen pillar (spec) means generated content needs meshletize + LOD +
  chunking **at runtime, in-engine** — so the cook logic must be a library,
  not a tool binary. The server sharing the generation pipeline points the
  same way.

## Decisions (locked with user, 2026-07-22)

- **Bake library, thin CLI**: `sandbox/assetbake/` (asset formats are game
  policy → sandbox-side, per repo conventions). Absorbs what today lives in
  `gltf_loader` (parse/flatten/tangents) and `meshlet_builder` (meshletize +
  LOD chain): those become bake-library code with the runtime keeping only the
  cooked reader + upload. A small CLI target (`.#cook`) cooks source glTFs.
  Keep the code procgen-shaped: the core entry point is
  `bake_scene(vertices, indices, draws) -> cooked blob` with the glTF importer
  as one front-end — procgen will call the same entry with generated arrays.
- **Cooked scene format** (one cooked file per source glTF; the engine's
  existing multi-file merge consumes N cooked scenes):
  - Header: format version, source content hash, counts/offsets. Little-endian
    raw structs, mmap-friendly section layout; every table 16-byte aligned.
  - Sections: vertex stream (48B `String::Vertex`, grouped per-draw so the
    streamer suballocates ranges directly), meshlet table (GpuMeshlet),
    meshlet-vertex remap, meshlet-triangle words, per-draw table (bounds, LOD
    ranges, material index, transform), material + texture tables (texture
    entries reference the KTX2 paths).
  - **NO index buffer**: meshlets fully replace indices at runtime (the GPU
    index heap is already deleted; CPU indices only ever fed meshletize, which
    now happens at cook). Indices exist transiently inside the bake.
  - Deterministic output: same source bytes -> byte-identical cooked file
    (needed for CI and the future client/server shared-generation contract).
- **Spatial chunking in the bake**: draws exceeding a meshlet budget
  (`kChunkMaxMeshlets`, start ~256, make it a bake parameter) split spatially
  before meshletize — recursive median split on triangle centroids (largest
  AABB axis) until under budget. Each chunk becomes its own draw with tight
  bounds and its OWN LOD chain. Chunk order must be deterministic (split
  recursion order), preserving stable draw ordering frame-to-frame.
  Material/transform duplicate per chunk (cheap, 216B DrawInfo each).
- **Staleness + fallback**: a sidecar manifest (source path, size, mtime,
  content hash). CLI re-cooks stale entries only. At load, the engine checks
  the manifest (fast path: size+mtime; hash only when those changed); if the
  cooked file is missing or stale it **cooks in-process via the library**
  (log a WARN with the CLI hint) and writes the cooked file next to the
  source (cooked outputs are gitignored, like sponza itself).
  This REPLACES `~/.cache/string/meshlets` — delete that cache path
  (the cooked file IS the cache; `string::core::user_cache_dir` stays for
  shaders).
- **Textures unchanged**: the KTX2/BC7 cook stays as-is this brief. The CLI
  may shell out to / mention `tools/cook_textures.sh`, but do not rewrite it.
- **Engine loader**: `GeometryPass` consumes cooked scenes (reader + upload +
  streamer registration). The synthetic quad injection (STRING_TRANSP_TEST)
  and the crowd instancing keep working — they operate on the loaded tables,
  not on glTF.

## Milestones

1. Bake library extraction: `bake_scene()` (flatten/tangents/meshletize/LOD
   moved in), cooked format writer + reader, gtest round-trip (write → read →
   compare tables) + determinism test (cook twice → identical bytes).
2. Engine loads cooked scenes end-to-end (in-process cook fallback when
   stale/missing); `~/.cache` meshlet cache deleted. **Parity gate: interior +
   exterior captures byte-identical (AE=0) vs pre-brief baselines** (chunking
   OFF — pure refactor at this point). Report cold + warm load times.
3. CLI (`.#cook`) + manifest incremental re-cook; cook the three Sponza packs;
   flake check target proving the cook runs.
4. Chunking ON (bake parameter default on): ivy splits into tight-bounds
   chunks. Report: chunk count, [mesh-cull]/[shadow-cull] stats and
   [frametime] on interior + exterior + crowd cameras before/after. Captures
   compared before/after chunking — differences must be imperceptible (small
   AE from draw-order changes is acceptable ONLY if eyeballed captures show
   no artifacts; z-fighting flicker across frames is a FAIL — coplanar
   surfaces must keep a stable order).

## Acceptance

- With cooked assets present: NO fastgltf/MikkTSpace/meshopt work at load;
  cold load of the 3-pack scene reported (target: a few seconds, dominated by
  vertex upload); identical behavior on a fresh user account (no cache
  warming).
- Milestone-2 parity AE=0; milestone-4 interior frame time materially
  recovered from the ivy regression (report the number — expectation is back
  toward ~5 ms on the 780M; state what was achieved).
- Determinism test green in `nix flake check`; validation clean; hot shader
  reload unaffected; F/C/O/L toggles + freeze behavior unchanged.
- NEEDS VISUAL VERIFY: post-chunking flythrough (ivy + curtains look
  unchanged, no popping/flicker), plus the milestone-4 capture pairs.
