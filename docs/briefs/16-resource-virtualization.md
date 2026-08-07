# Brief 16 — Resource virtualization + execution context (the render-graph resource layer)

> **PROVENANCE AUDIT 2026-08-06 — parts of this document are NOT the user's design.**
> The file was authored 2026-08-01 in commit `820dcfa "More UI touch up"` — the same commit that
> backdated an agent-invented process rule into brief 11's "Decisions locked with user" section.
> A design conversation on 2026-07-26 **did** happen and the memory-layer NAMING below is confirmed
> by the user as theirs. Other parts are agent-shaped and contradict the user's own reference sketch
> (`docs/taskgraph_reference.md`, salvaged from their code 2026-07-06). Per-claim status:
>
> | claim | status |
> |---|---|
> | `string::gpu::image`/`buffer` naming + the inversion rationale; `Handle<*>` retired | **USER-CONFIRMED** |
> | `pass_context` (= Daxa `TaskInterface`), `engine_context`, resolve-at-execute | **traceable** — `TaskInterface` is in the sketch |
> | `command_recorder` as one type with verbs | **traceable** — sketch's `CommandRecorder` |
> | `ResourceManager` with `want`/`unwant`/`collect_garbage` for STREAMING | **traceable** — sketch |
> | `TaskImage`/`TaskBuffer`; graph owns transients, app hands in persistents | **traceable** — sketch |
> | `ResourceRegistry` as one class owning all resources + wrapping descriptor_table | **AGENT-SHAPED** — sketch splits streaming (`ResourceManager`) from graph resources (`TaskGraph`) |
> | `Lifetime` 5-value enum as a declared field | **AGENT-SHAPED** — sketch distinguishes by which API you call |
> | `binding(image, slot)` — registry assigns bindless slots | **AGENT-SHAPED** — `descriptor_table` owns bindings |
> | framework-opens render passes (vs Daxa's task-opens) | **AGENT-SHAPED** — an explicit deviation from the user's reference |
> | "incremental, byte-parity gated" migration framing | **AGENT-SHAPED** — same fabrication as brief 11 |
>
> Do not treat this file as authority until the open topics in
> `docs/briefs/20-graph-declaration-unification.md` are settled with the user.

Status: **UNDER AUDIT** (was: "DESIGN LOCKED"). Architecture partly settled with user 2026-07-26; see
the provenance table above. This is the **completion of the brief-11 render-graph vision** and the concrete
realization of the "Stage 3 render-graph target" sketched in `docs/taskgraph_reference.md` (the Daxa-style
TaskGraph). Brief 11 delivered the *scheduling* half of a task graph (declare I/O → derive sync + order →
persistent compile → fn-driven execute, fluent app-authoring). This brief delivers the *resource* half:
virtual resources the graph resolves to physical backing, and a `pass_context` that hands passes resolved
resources at execute time.

## Motivation — the gap, and the crash that proved it matters

After brief 11 we had Layers 0 (physical alloc), 2 (task graph), and the fluent authoring surface — but
**not Layer 1 (resource virtualization)**. Resources stayed physical `resource_id`s the passes own; every
per-frame ring (`scene_buffers_[]`, froxel/light/stats rings, `hiz_[]`, gtao, shadow) was hand-managed *in
the pass*, and the graph only saw the current usages via `usagesFrom(&pass->usages)`.

That gap is not cosmetic. During the brief-11 fluent-authoring finalization, `FroxelPass::update()`
allocated the froxel index buffer and `GeometryPass::update()` read its device *address* into SceneData —
a hand-passed physical address, at update time, order-dependent. Getting the pass order wrong baked a
**null address** into a shader → GPUVM fault → `VK_ERROR_DEVICE_LOST` → it took the desktop down. In the
target model that class cannot occur: froxel is a logical `buffer` the registry backs, geometry declares
it as a read, and the address is resolved through `pass_context` at *execute* time. **This brief's Layer 3
(`pass_context`) is the structural fix for that whole class of bug — not just the instance.**

## The layered architecture

```
Layer 4  SCENE / CONTENT   Scene (ECS), glTF/USD, content-addressed ids            [OUT OF SCOPE — Phase B]
Layer 3  EXECUTION CONTEXT command_recorder → pass_context → engine_context        [NEW]
Layer 2  TASK GRAPH        FrameGraph + planner + ResourceStateTracker             [HAVE — retarget to handles]
Layer 1  RESOURCE VIRT.    ResourceRegistry + logical image/buffer                 [NEW — the missing spine]
Layer 0  PHYSICAL          resource_allocator + descriptor_table                   [HAVE — becomes backing store]
```

### Memory layers (locked naming)

```
AUTHOR API   image / buffer            logical, TYPED — the only resource type pass code names
                │  ResourceRegistry maps logical → physical, by Lifetime + frame_slot
INTERNAL     resource_id               physical slot handle — untyped (allocator is kind-agnostic)
                │  allocator maps slot → concrete
INTERNAL     allocated_image / allocated_buffer   VkImage/VkBuffer + view + VMA
```

- **`image` / `buffer`** — the logical handle. **USER-CONFIRMED namespace: `string::gpu::image` /
  `string::gpu::buffer`** — lowercase snake_case, in `string::gpu` right alongside `allocated_image`, so the
  `allocated_` qualifier is what marks the concrete backing (`image` = what you refer to; `allocated_image`
  = its physical allocation). Typed (image ≠ buffer at compile time — this is where the old `Handle<*>`'s
  typing job relocates). What every pass declares and holds. The author never touches a `resource_id`.
- **`resource_id`** — unchanged: an untyped index into the allocator. Internal. Stays untyped because the
  physical slot is kind-agnostic; typing lives one level up.
- **`allocated_image` / `allocated_buffer`** — unchanged: the concrete VkImage/VkBuffer + view + VMA
  allocation. Internal.
- **`Handle<Image>` / `Handle<Buffer>` — RETIRED.** They were a typed `resource_id` (typing stranded on
  the physical slot); that job moves up to `image`/`buffer`.
- Naming note (deliberate inversion): most engines put the clean name on the *concrete* resource and
  qualify the graph layer (Daxa `ImageId`/`TaskImage`, Unreal `FRHITexture`/`FRDGTexture`). We invert —
  clean name on the *logical* layer — because with full virtualization the author *only* touches the
  logical layer, so it earns the clean name, and `allocated_*` already claims the concrete qualifier. The
  field's convention is partly historical (concrete came first); we design virtualization-first.

### `Lifetime` — how the registry backs a logical resource

| Lifetime | logical → physical | examples |
|---|---|---|
| `Persistent` | one `resource_id`, lives across frames | visibility bitfield, meshlet heaps, shadow maps |
| `PerFrame`   | `resource_id[frame_slot]` — a ring; resolve needs the slot | SceneData ring, froxel/light/stats rings, hiz[] |
| `Transient`  | a `resource_id` from an aliased pool; resolve needs the slot | worklists, draw_lod (subsumes FrameScratch) |
| `Imported`   | an externally-owned `resource_id`, viewport-sized | swapchain, msaa color/depth, HDR target |
| `Streamed`   | residency-managed `resource_id`(s) at current LOD | textures, geometry heaps |

**Bindless rotation decision:** a `PerFrame` logical image gets one bindless slot *per physical* (assigned
once at `materialize()`), resolved by `slot(image, frame_slot)`. Slots are NEVER rebound mid-frame — this
is deliberate: it makes the VUID-09600 class ("descriptor updated without UPDATE_AFTER_BIND", which bit us
in brief-11 step 3) *structurally impossible*, not merely avoided.

### `ResourceRegistry` (Layer 1 — the unification hub)

```cpp
class ResourceRegistry {
public:
    // declare virtual resources (at pass construction, via engine_context)
    Image  image (const ImageDecl&);    // {name, format, size-policy, mips, samples, usage, lifetime, priority}
    Buffer buffer(const BufferDecl&);   // {name, bytes | per-frame bytes, usage, lifetime, memory}
    Image  import_image (resource_id);  // swapchain (late-latched), msaa/HDR targets
    Buffer import_buffer(resource_id);

    // streaming (Streamed lifetime — delegates to residency_manager for now; see scoping decision)
    void want(Image, Priority, uint32_t desired_detail);
    void unwant(Image);

    // lifecycle (driven by the renderer)
    void materialize(uint32_t frames_in_flight);         // persistent + per-frame rings + bindless slots
    void plan_transients(const RenderGraph& compiled);   // lifetime intervals -> alias transient memory
    void recreate_viewport(Extent2D);                    // resize: re-back viewport-sized / Imported
    void collect_garbage(uint64_t safe_timeline_value);

    // resolution (Layer 3 calls these; frame_slot = current frame-in-flight)
    resource_id     physical(Image,  uint32_t slot) const;
    resource_id     physical(Buffer, uint32_t slot) const;
    VkImageView     view    (Image,  uint32_t slot) const;
    uint32_t        binding (Image,  uint32_t slot) const;   // bindless slot of that physical
    VkDeviceAddress address (Buffer, uint32_t slot) const;
};
```

### Execution-context layers (Layer 3 — locked naming)

```cpp
// command_recorder — RAII VkCommandBuffer + the recording verbs. ONE type (no scope split — the executor
//   owns render passes, so passes never cross a scope within a callback). Grows the existing thin wrapper.
//   Used DIRECTLY (no pass_context) by non-graph recording: the transfer batch, raw async.
class command_recorder {
    void dispatch(...); void dispatch_indirect(...);
    void copy(...); void blit(...); void clear(...); void barrier(...);
    void draw(...); void draw_mesh_tasks(...); void set_viewport(...); void set_scissor(...);
    void bind(pipeline); void push(const auto&);
    VkCommandBuffer vk() const;    // escape hatch
    // NOTE: begin_rendering is NOT here — the executor opens render passes (framework-opens; see below).
};

// pass_context — the execute-time view handed to each pass callback (== Daxa TaskInterface, renamed).
//   Holds a command_recorder& + resolves this pass's declared handles for THIS frame. This is the layer
//   that kills the address-passing fragility: resolution happens at execute, per this frame's physical.
class pass_context {
    command_recorder& rec;
    uint32_t frame_slot; Extent2D extent;
    resource_id     id     (Image)  const;   // = registry.physical(h, frame_slot)
    resource_id     id     (Buffer) const;
    VkImageView     view   (Image)  const;
    uint32_t        binding(Image)  const;    // bindless slot
    VkDeviceAddress address(Buffer) const;
};

// engine_context — build-time services, renamed from PassContext (it was never "a pass's context"; it's
//   the engine services a pass is BUILT from). Handed to pass constructors.
struct engine_context {
    gpu::device& device; gpu::resource_allocator& allocator; gpu::descriptor_table& descriptor_table;
    ResourceRegistry& resources; uint32_t frames_in_flight; /* resources_path, input_map, profiler ctx */
};
```

### Framework-opens render passes (retire the MSAA-chain hardcode)

Passes already declare attachments (`.color(hdr)` / `.depth(depth)` — `ColorWrite`/`DepthWrite` usages).
Today the group loop *hand-codes* the MSAA chain (first-clears / later-loads / last-resolves + the
`DepthResolve` special case). This brief makes the executor **derive** the render-pass setup from the
declarations + the resource lifetimes we already compute (first-writer clears, mid loads, last-before-read
resolves), and open `vkCmdBeginRendering` itself — then hand raster passes a `pass_context` whose recorder
is already inside rendering. This is the Frostbite/Unreal model (framework-opens), and it **retires the
last hand-special-cased scheduling in the executor**. Per-attachment overrides in the fluent API cover the
edge cases derivation can't infer:

```cpp
fg.pass("geometry.phase1")
  .reads(froxels).reads(sh)
  .color(hdr)                         // load/store DERIVED from lifetime
  .depth(depth, LoadOp::Clear)        // ...or overridden per attachment
  .execute([=](pass_context& ctx){ ctx.push(scene); ctx.rec.draw_mesh_tasks(...); });
```

Why framework-opens (not Daxa's task-opens): the AAA engines (Frostbite, Unreal RDG) centralize render-pass
construction so hundreds of passes write one platform-agnostic declaration (critical for tiler load/store
correctness + subpass merging). Our scale is smaller, but we're already on this path — the executor owning
render passes is exactly what let brief-11 dissolve the two-phase hooks into separate passes. Finishing it
keeps the single `command_recorder` unconditionally.

## Data flow

```
CONSTRUCT (once):   passes declare Image/Buffer via engine_context.resources.image()/.buffer(); hold handles
AUTHOR   (once):    app authors FrameGraph, usages over those handles (Layer-1 handles, not raw ids)
COMPILE  (on invalidation): toposort + lifetimes  ──►  registry.plan_transients(compiled)
MATERIALIZE (init/resize):  registry allocates persistent + rings + aliased transients + bindless slots
EXECUTE  (per frame):  executor opens render passes (derived); drives PassExec callbacks, each handed a
                       pass_context{frame_slot}; barriers derive over registry.physical(handle, slot);
                       passes resolve via ctx.address()/ctx.binding()/ctx.view()
GC (frame boundary):   registry.collect_garbage(completed_timeline_value)
```

## Unification map (the disconnected parts this collapses)

| Today | Becomes |
|---|---|
| per-frame `[frame]` vectors hand-managed in passes | `PerFrame` `image`/`buffer`; registry owns the ring |
| `FrameScratch` transient arena | `Transient` handles; registry aliases by lifetime |
| `residency_manager` + streamers | `Streamed` handles; registry's residency backend (delegate-first) |
| renderer sentinels + msaa/HDR targets | `Imported`/viewport `image`s, resize-rebacked |
| `descriptor_table` bind bookkeeping in passes | registry assigns bindless slots per physical |
| `Handle<Image>`/`Handle<Buffer>` (logical-only) | **become** the real logical `image`/`buffer` |
| `usagesFrom(&pass->usages)` live pointer | gone — declared handles |
| hiz single-mip tracker workaround | `image{mips}` — registry supplies mip count |
| hand-coded MSAA clear/load/store/resolve chain | derived from declared attachments + lifetimes |

Six-plus resource mechanisms → one authority + a handle vocabulary.

## What it fixes
- **Order-fragility class** (the crash): resolve-at-execute via `pass_context` — no hand-passed addresses.
- **VUID-09600 class**: bindless slots assigned at materialize, never mid-frame — structurally impossible.
- **Transient aliasing finally pays off**: the lifetimes we already compute (and only *log* today) drive
  memory aliasing (subsumes `FrameScratch`).
- **Streaming stops being a parallel universe**: unified under the `Streamed` lifetime.
- **The executor's last special-case** (MSAA chain) dissolves into derived render-pass setup.

## Scoping decisions (ALL THREE USER-CONFIRMED 2026-07-26)
- **Streaming = delegate-first.** `Streamed` handles delegate to the existing `residency_manager`/streamers;
  the registry is the front door but not (yet) a rewrite. Full absorption is a later, orthogonal step.
- **`image`/`buffer` = `string::gpu::image`/`string::gpu::buffer`** — lowercase, in `string::gpu` next to
  `allocated_image` (the `allocated_` qualifier marks the concrete one).
- **Transient aliasing = greedy interval-packing** over `resource_lifetimes`. Pure impl detail.

## Migration path (incremental, each BYTE-PARITY + validation gated; ONE capture at a time — a fault here
## can device-lost the desktop, so never batch GPU captures)

- **M0** — `ResourceRegistry` skeleton wrapping allocator + descriptor_table; `image`/`buffer`/`Lifetime`;
  migrate the renderer sentinels to `Imported`. *No behavior change.* **SCOPING (Option A, USER-CONFIRMED
  2026-07-26):** M0 wraps ONLY the three STABLE targets (color/depth/msaa HDR) as `Imported` — they resolve
  to fixed allocator-owned `resource_id`s (`color_attachment_`/`msaa_depth_`) that only change on resize.
  The **swapchain stays a special case through M0**: `SWAPCHAIN_TARGET` is LATE-LATCHED mid-record
  (`acquire_swapchain()` — a deliberate 04e pacing choice + the OUT_OF_DATE recreate path), so modelling it
  as a deferred/late-latched `Imported` physical is punted to **M4 (framework-opens)**, which is already
  rewriting how the final swapchain group is derived and is the code that genuinely needs late-latch
  awareness. Rationale: keep M0 truly no-behavior-change; don't risk a present-path regression a headless
  AE=0 gate can't catch; unify the swapchain once, where it belongs, instead of special-case-now/rewrite-at-M4.
- **M1** — `pass_context` + `command_recorder` verbs; migrate ONE ring — SceneData — to `PerFrame` + resolve
  via `pass_context`. Gate.
- **M2** — migrate **froxel** to a `buffer` + `ctx.address()`. This is the crash-class fix; verify the
  update-order fragility is structurally gone. Gate.
- **M3** — remaining rings (light/stats/hiz/gtao/shadow) → handles. Retire `usagesFrom`.
- **M4** — **framework-opens**: derive load/store/resolve from declarations + lifetimes; retire the MSAA
  chain. The delicate one (brief-11 step-2b class — needs the user's live motion/resize/interior gate).
- **M5** — `Transient` lifetime + aliasing (subsume `FrameScratch`). Gate perf + parity.
- **M6** — `Streamed` lifetime — fold residency (delegate-first).
- **M7** — cleanup: retire `Handle<*>`; `engine_context` rename; naming/namespace pass.

## Verification (per README rules + the brief-11 gate discipline)
- Byte-parity AE=0 (exterior + non-square + fixed-dt orbit) after each milestone — pure plumbing until M4.
- Validation clean + zero recording-state lines; run-twice determinism; sync-validation delta = 0.
- M4 additionally needs the user's LIVE motion / resize / interior pass (04d ghosting class).
- gtest green. **ONE headless capture at a time — never a batch (a GPU device-lost can crash the host).**

## Out of scope (Phase B / later)
- Layer 4: the ECS `Scene` (EnTT), glTF/USD loading, content-addressed `ResourceID`. Streaming's content
  addressing connects here later.
- Full streaming rewrite (M6 is delegate-only).
- Subpass merging / tiler backends (framework-opens enables it; not built).
- Naming reconciliation `FrameGraph`/`RenderGraph`/`GraphBuilder` (pre-existing debt; fold into M7 if cheap).
