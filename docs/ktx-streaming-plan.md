# Textures: KTX2/BC7 + asset streaming

## Context

Turning String into a usable game-client library. Today every glTF texture is decoded from
PNG/JPG on the CPU with stb_image at load (`sandbox/passes/geometry_pass.cpp`), which is the
dominant startup cost (~15s on Sponza) and a hard floor — see the `gpu-compressed-textures-required`
memory. All textures for a model are uploaded up front; there is no residency management.

This track has two phases. Phase 1 (KTX2/BC7) is self-contained and removes the load-time floor.
Phase 2 wires the residency machine already scaffolded in `string-engine/include/string/gpu/resource.hpp`
(`stream_status`, `resource_priority`) on top of it. Geometry/meshlet streaming comes later with
the mesh-shader rework.

## Decisions

- **Format: KTX2 cooked straight to BC7 + full mip chain on disk (SUPERSEDES the earlier
  UASTC+runtime-transcode decision below).** `ktx create --encode` can't encode raw BC7 (only
  `basis-lz | uastc`), but `ktx transcode --target bc7 --zstd` bakes a UASTC intermediate to a
  BC7+zstd file offline. Since String targets desktop Vulkan + BC7 only, we ship BC7 and the runtime
  never transcodes — it just uploads BC7 blocks (KTX2 deflates each mip level independently, so
  streaming reads/inflates only the levels it needs). This is what let Phase 2 drop the RAM cache /
  background-transcode machinery. `ktxTexture2_NeedsTranscoding` is checked at load and a UASTC→BC7
  `ktxTexture2_TranscodeBasis` fallback is kept for any `.ktx2` that isn't already BC7. See the
  `bc7-baked-on-disk` memory.
  - *Original decision (kept for context):* cook to UASTC + full mip chain and transcode to BC7 at
    runtime with `ktxTexture2_TranscodeBasis(tex, KTX_TTF_BC7_RGBA, 0)`. Chosen because `ktx create`
    can't encode BC7; abandoned once we realised `ktx transcode` bakes BC7 offline and the transcode
    was the dominant load cost + the whole reason a RAM cache was needed.
- **libktx lives only in the sandbox loader.** The engine's `TransferBatch` stays format-agnostic:
  it takes a raw blob + explicit per-level layout. Keeps the KTX dependency out of the engine core.
- **Keep the stb_image path as a fallback** for uncooked/embedded textures during dev. The loader
  prefers a cooked `.ktx2` sibling when present, else decodes via stb.

## Phase 1 — KTX2/BC7

### Build wiring
- `flake.nix`: add `pkgs.ktx-tools` to `packages.demo` `buildInputs` (libktx: `ktx.h`,
  `ktxvulkan.h`, `libktx.so`, CMake config `KTX::ktx`) and `nativeBuildInputs` (the `ktx` CLI, for
  cooking); add to the devShell too.
- `string-engine/subprojects/ktx.wrap`: CMake-method wrap for off-nix builds, `[provide] ktx = ...`.
- `sandbox/meson.build`: `dependency('ktx', method: 'cmake', modules: ['KTX::ktx'])`.

### Engine: `TransferBatch` (`string-engine/src/vulkan/transfer_batch.cpp` + header)
Add `upload_image_levels(data, total_size, std::span<const level_copy>, dst_image)` where
`level_copy = { VkDeviceSize offset; VkExtent3D extent; }`. Stage the blob once, transition all mip
levels UNDEFINED→TRANSFER_DST, issue one `vkCmdCopyBufferToImage` per level (row length 0 = tightly
packed; Vulkan does the block math from the BC7 format), then transition all levels →SHADER_READ.
No blit. Leave the existing blit-based `upload_image` for the stb fallback.

### Sandbox loader (`sandbox/passes/geometry_pass.cpp`)
- New `load_ktx2(path)`: `ktxTexture2_CreateFromNamedFile`; if `ktxTexture2_NeedsTranscoding`,
  `ktxTexture2_TranscodeBasis(..., KTX_TTF_BC7_RGBA)`. Returns handle + `ktxTexture2_GetVkFormat()`
  + level offsets (`ktxTexture_GetImageOffset`) and per-level extents.
- New `upload_ktx2(...)`: create the image with `format = GetVkFormat()`, `mip_levels = numLevels`,
  usage `TRANSFER_DST | SAMPLED` (no `TRANSFER_SRC`), call `upload_image_levels`, bind bindless slot.
- Prefer a `.ktx2` sibling of the resolved texture path; else the existing stb path.

### Cook (`string_cook`, string-asset-tools; superseded `tools/cook_textures.sh`)
Two stages (see the superseding format decision above): `ktx create --encode uastc --generate-mipmap
--format R8G8B8A8_SRGB --assign-tf srgb` (linear `R8G8B8A8_UNORM`, no srgb tf, for normal /
metallic-roughness) into a throwaway UASTC temp, then `ktx transcode --target bc7 --zstd 18` into the
shipped BC7 `.ktx2`. Produces `.ktx2` siblings next to the glTF's source images. Run once per asset
(assets are gitignored / external).

### Verify
Cook Sponza's textures, `./run.sh`; startup drops from ~15s to sub-second (compare the existing
`[load]` log lines), textures render identically, mips still filter with distance.

## Phase 2 — texture residency streaming (later)

Full plan: **`docs/phase2-texture-streaming.md`**. In short: a `TextureStreamer` driving the
`stream_status` machine — read BC7 mips per want (no transcode; whole-blob load freed after staging,
re-read on demand), stream per-mip *uploads* with an always-resident coarse tail, desired-mip from
per-draw screen coverage (reuse the `GltfDraw` AABBs), VRAM budget + least-visible eviction. Main
engine change: `TransferBatch` runs its async ring **across frames** (per-frame `flush()` +
completion tracking) instead of a one-shot `wait_idle()` before frame 0.

## Later
Geometry/meshlet streaming reuses the same residency machine, sequenced after the mesh-shader rework.
