# Phase 2 — Texture residency streaming

## Context

Phase 1 (KTX2/BC7) took Sponza load from ~15s to ~1.9s. The remaining floor (~1.4s) is the
texture pipeline uploading **all 72 textures, all mips, up front**: the UASTC→BC7 transcode plus
a serial ~1.5 GB staging memcpy in `GeometryPass`'s constructor, drained once by the renderer's
`TransferBatch::wait_idle()` before frame 0. Streaming removes this structurally — only what's
visible (and only at the detail it needs) is resident — and is the prerequisite for scenes larger
than VRAM. The residency state machine is already sketched in `string-engine/include/string/gpu/resource.hpp`
(`stream_status{WANTED, STREAMING, CACHED, UNWANTED}`, `resource_priority{LAZY, IMMEDIATE}`); this
phase makes it real. See [[library-roadmap]] and `docs/ktx-streaming-plan.md` (Phase 1).

## Goal

- Render the full scene within a frame or two of load, at low detail, without uploading every mip.
- Stream finer mips on demand by on-screen need; keep GPU texture memory under a fixed budget with
  eviction.
- No visible sampling of not-yet-resident mips (no black/garbage texels while streaming).

## Key distinction: transcode vs upload (RESOLVED — BC7 is now baked on disk)

Originally the cooked files held UASTC, and Basis transcodes **whole-texture** to BC7
(`ktxTexture2_TranscodeBasis`), not per-mip — so we couldn't cheaply transcode "just the coarse
mips," which forced a whole-texture-transcode-then-cache-the-blob design and all its RAM-budget /
re-transcode complexity.

That's gone: textures are now **baked to BC7 on disk** (`ktx transcode --target bc7 --zstd`, see
`docs/ktx-streaming-plan.md`), so there is **no runtime transcode**. Streaming a mip is a plain
read + upload. KTX2 zstd supercompression is per-level, so the loader reads/inflates only the levels
it needs (a decompress, not a transcode). Consequences:

- **No RAM cache, no background transcode pool.** A stream loads the whole BC7 file, records the mip
  uploads it needs (the `TransferBatch` copies the bytes into staging immediately), then frees the
  file — so RAM holds only the blobs of in-flight streams, not every texture. A finer stream later
  re-reads the file (cheap: a zstd inflate; a UASTC→BC7 transcode only on the fallback path).
- **`phys_base`** tracks the finest mip already physically uploaded. v1 eviction is minLod-only (no
  VRAM reclaim), so physical mips survive — re-wanting detail after eviction uploads nothing.
- A **UASTC→BC7 transcode fallback** remains for any `.ktx2` that isn't already BC7 (un-baked /
  hand-supplied), so uncooked assets still load. This is the "virtual texture lite" shape, minus the
  caching machinery.

## Architecture

### `TextureStreamer` (new — lives in the sandbox, like the KTX loader)
Owns residency for the model's textures. Engine stays generic; the streamer uses engine primitives
(`TransferBatch`, `descriptor_table`, `resource_allocator`, `job_system`). Per texture it holds a
`Texture`:
- source KTX path; mip count; base extent; the chosen BC7 `VkFormat`.
- the GPU image (`resource_id`), created up front at **full** mip count but only partially populated.
- `phys_base` — finest mip index physically uploaded so far (no long-lived blob; the file is loaded
  per stream and freed after staging — see the key-distinction section).
- in-flight-load state: `loading` + `future` + `pending_base` (finest level the load must upload).
- `resident`/`desired` detail live in the engine's `residency_manager`, not here (this is just the
  provider). Detail = resident mip levels from the coarsest; base_mip = mip_count − detail = minLod.
- bindless slot (stable for the image's lifetime; the slot never changes as mips stream in/out).

### Residency lifecycle (drives `stream_status`)
1. **Init**: create each image at full mip count, upload only the coarsest N mips (tiny), bind the
   slot, mark `CACHED` at a coarse `resident_base_mip`. Scene renders immediately (blurry).
2. Per frame: the feedback pass computes `desired_base_mip` per texture. If finer than resident →
   `WANTED`. The streamer enqueues the transcode (if not cached) + the finer-mip upload on the
   `job_system` / `TransferBatch` → `STREAMING`. On completion → `CACHED` at the new base; lower the
   sampler's `minLod` so the new detail becomes visible.
3. Under budget pressure, textures not wanted at their current detail → evict fine mips (raise
   `resident_base_mip`, `minLod`) → `UNWANTED`/coarser `CACHED`. Coarse tail is never evicted.

### Feedback signal (start simple)
- v1: CPU, per frame, in the pass `update()`. For each `GltfDraw` (world AABB already computed for
  culling), estimate projected screen coverage from the camera and map texel density → desired mip.
  Aggregate per texture (a texture used by multiple draws takes the finest desired). Cheap: 405 draws.
- Later: GPU sampler feedback (`VK_EXT_image_footprint` / feedback maps) for exact per-texel need.
  Explicitly out of scope for v1.

### Sampling partial-resident images
The image has all mip levels allocated but only `resident_base_mip..N-1` populated. Sampling must
never read a finer (lower-index) level than resident. Clamp with **`minLod = resident_base_mip`**.
`minLod` is a sampler property, so streamed images need a per-image sampler whose `minLod` the
streamer updates as mips arrive/evict (today `allocated_image.sampler` is one shared config — this
phase gives streamed images an adjustable-minLod sampler). Alternative considered: shift the image
view's `baseMipLevel` (rejected — renumbers mips and churns the view/bindless slot on every change).

## Engine change: `TransferBatch` runs across frames

Today `TransferBatch` records all uploads at construction and the renderer drains it once with
`wait_idle()` before frame 0. Streaming needs ongoing, non-blocking, per-frame uploads:
- Per frame: `flush()` the current batch (submit, don't wait); never `wait_idle` in steady state.
- The async ring + timeline semaphore already bound resident staging and reclaim it on slot reuse
  (`begin_if_needed`); expose **completion** so the streamer can flip `STREAMING→CACHED` and update
  `minLod` only after the GPU has the data (track each upload's timeline signal value; poll
  `vkGetSemaphoreCounterValue`).
- Per-mip barriers: uploading level `k` into a live, sampled image must transition just that
  subresource UNDEFINED→TRANSFER_DST→SHADER_READ while other levels stay readable. The mip-range
  fields on `ImageTransition` (`base_mip`/`level_count`, added in Phase 1) already support this;
  `upload_image_levels` needs a variant that targets a subrange rather than assuming level 0..N.

## Milestones (incremental, each independently verifiable)

1. **Cross-frame TransferBatch.** Add per-frame `flush()` + completion tracking; keep the one-shot
   `wait_idle` path for non-streamed uploads (vertex/index/white). Verify: no regression, textures
   still load (as a single up-front batch that now completes over a few frames).
2. **Coarse-tail init + minLod.** Upload only the coarsest N mips up front; give streamed images an
   adjustable-minLod sampler set to the coarse base. Verify: scene renders instantly and blurry;
   the up-front upload memcpy (and load time) drops sharply.
3. **Streamer + CPU feedback.** Add `TextureStreamer`; per-frame desired-mip from AABB screen
   coverage; stream finer mips in via TransferBatch, lower `minLod` on completion. Verify: flying
   toward a wall sharpens its texture within a frame or two; `stream_status` transitions logged.
4. **Budget + eviction.** VRAM budget; LRU/least-visible eviction of fine mips (coarse tail pinned).
   Verify: force a small budget, confirm memory stays capped and distant textures drop to coarse
   without artifacts.

## Risks / open questions

- ~~**RAM cache of transcoded BC7.**~~ RESOLVED by baking BC7 on disk (see the key-distinction
  section): no blob is cached, streams free the file after staging, and a finer stream re-reads the
  (cheap, per-level-zstd) file. The RAM-vs-re-transcode tradeoff no longer exists.
- **Per-image samplers** interact with the bindless table (sampler baked into the image's descriptor
  today). Confirm the descriptor write path can carry a per-image sampler, or move to a separate
  sampler array / combined-image-sampler update on `minLod` change.
- **Cook coarse mips.** Current cook (`tools/cook_textures.sh`) already emits full chains via
  `--generate-mipmap`; no cook change needed for mip streaming. A separate small "always-resident"
  file is an alternative to picking a coarse-mip cutoff — decide in milestone 2.
- **Geometry streaming** (meshlet clusters) reuses this residency machine but is sequenced later
  with the mesh-shader rework — out of scope here.

## Verification (end to end)

Fly through Sponza: (1) load time drops further (coarse tail only up front — compare `[load]`
lines); (2) full scene visible immediately, sharpening as you approach surfaces; (3) with a small
forced VRAM budget, GPU texture memory stays capped and no missing-mip artifacts appear; (4)
`stream_status` transition logs match what's on screen.
