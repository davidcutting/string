# String — Resource Layer Plan

Grounded in the current code. Principle: **the per-frame unit is `Frame`; the
reclamation clock is the existing timeline. Anything new hangs off those two and adds
no parallel machinery.** Add only what a real consumer needs.

## What the code already has (verified, don't rebuild)

- **Timeline + frame value:** `frame_semaphore_` (timeline semaphore) + monotonic
  `frame_count_`. Each `Frame` slot records the value it last signaled in `frame_id`;
  `begin_frame` waits on it (`renderer.cpp:145`), `end_frame` signals `frame_count_`
  (`renderer.cpp:398`).
- **Per-frame-ring deferred deletion:** `Frame::garbage_collector` (a `DeletionQueue`)
  is flushed in `begin_frame` immediately after that slot's timeline wait
  (`renderer.cpp:155`). This is a correct, timeline-gated reclaim — the mechanism the
  earlier drafts tried to reinvent. **Nothing pushes into it yet**; it is ready
  infrastructure.
- **Persistent allocation:** `ResourceAllocator` (VMA, returns `ResourceID`). Only
  consumers today are the color/depth attachments. Every destroy runs under
  `vkDeviceWaitIdle` (teardown + `presenter.resize`), so immediate free is safe today.

## What is dead / unused right now

- `global_descriptor_table_` (bindless `DescriptorTable`) — constructed
  (`renderer.cpp:37`), never bound.
- `Frame::descriptor_table` (`DescriptorAllocatorGrowable`) — only `init()`/destroyed
  (`renderer.cpp:72,113`), never used to allocate anything.
- No transient allocator exists; nothing needs one.

The triangle uses no descriptors at all (`hello_triangle_pass.cpp:13`).

---

## Deferred until a consumer exists (design only — do NOT build yet)

Each of these is a small hook into the **existing** `Frame`/timeline, not a new
subsystem. Written down so the future work fits the model; none is justified today.

- **Safe mid-frame destroy.** The first time a resource is destroyed during a live
  frame (streaming/dynamic), route it through the ring instead of freeing immediately:
  ```cpp
  frames_[current_frame_].garbage_collector.push_function(
      [this, id]{ allocator_.destroy_resource(id); });
  ```
  Immediate `destroy_resource` stays for idle-gated teardown/resize. No new type.
  (Enabled by the already-applied `DeletionQueue` `std::move` fix.)

- **Transient per-frame buffers.** When a pass first needs CPU→GPU per-frame data, add
  a bump arena as a `Frame` member and reset it in `begin_frame` alongside the existing
  `recorder.reset()` / `garbage_collector.flush()`. It returns a plain `AllocatedBuffer`
  (bump offset in `allocation_info.offset`, host ptr in `allocation_info.pMappedData`).
  No new tick, no `Timeline` type — it rides the `Frame` that is already reset each
  cycle. "Transient" is the contract of this allocator (recycles on frame reset, exposes
  no destroy), not a distinct return type.

- **`ResourceManager` facade / `AssetID` streaming.** Only when a second resource
  producer or actual asset streaming appears. Until then the renderer owning
  `allocator_` + `DescriptorTable` directly is already cohesive; a facade would add
  indirection with no consumer.

## Explicitly rejected (reinvent existing machinery)

`Timeline` wrapper, `FrameContext`, value-tagged `deferred_` free lists, per-subsystem
`next_frame`, typed `Handle<>`, `TransientBuffer`, `ResourceKind`. The `Frame` +
timeline ring already provides the clock and the reclaim; `ResourceType` and
`AllocatedBuffer` already provide the vocabulary.

## Already applied

- **Retired the growable descriptor system** (vkguide's `DescriptorAllocatorGrowable`
  + `DescriptorLayoutBuilder`/`DescriptorWriter`): deleted
  `descriptor_allocator_growable.{hpp,cpp}`, removed from `meson.build`, dropped
  `descriptor_table` from `Frame` and its `init`/`destroy_pools` calls in
  `renderer.cpp`. `Frame` is now `{ frame_id, garbage_collector, recorder }`. Bindless
  `DescriptorTable` is the sole descriptor model. `nix build` + `nix flake check` green.
- `DeletionQueue::push_function` `std::move` fix.
- Step-1 signature syncs (device/instance handles by value, `copy_data_to_buffer`
  `const void*`).
