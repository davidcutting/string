# Brief 20 — One declaration, over logical handles

Status: **APPROVED — landing as one change (big bang), user-directed 2026-08-06.** Scoped 2026-08-06;
naming (snake_case) and the app seam confirmed with the user the same day. Supersedes the agent-shaped parts of
brief 16 (see its provenance table) and the "what landed" section of brief 11.

## The rule this restores

> A pass declares **what** it touches; the renderer derives ordering, barriers, layouts, attachments
> and queue placement. — 04e
>
> The author never touches a `resource_id`. — 16 (user-confirmed naming section)

## What is actually wrong

Measured 2026-08-06:

| | count |
|---|---|
| declarations through the fluent API | **1** |
| declarations through raw `usages.push_back({id, Access, stage})` | **33** |
| hand-written `VK_PIPELINE_STAGE_2_*` masks in pass code | **112** |
| graph-created virtual resources used by real passes | **0** |

**Root cause — two disconnected declaration channels.** Fluent authoring feeds the planner (ordering,
lifetimes). `Pass::usages` feeds barrier derivation. Neither populates the other, so a pass must
declare twice, in two formats, to get both. Only the raw channel can name a per-frame resource, and
only the raw channel governs correctness — so that is the one everyone writes.

Everything else follows: the stage masks, the order-dependent RAW/WAR edges (the planner sees a
sparse subset of real usages), the dead degrade policy, three hand-rolled degrades.

## Vocabulary (user's, confirmed 2026-08-06)

- **`string::gpu::image` / `string::gpu::buffer`** — the logical resources. These *are* Daxa's
  `TaskImage`/`TaskBuffer` under our names. There is no additional task-resource layer.
- **persistent** — lifetime owned **outside** the graph; the graph manages *usage* only.
- **transient** — frame-scoped; lifetime owned **by** the graph; aliasable.
- These two need **no enum**. A resource backed by a swapchain image, a per-frame ring, or the
  streamer is *persistent* in all three cases — the graph does not care how the owner backs it.
- **`pass_context`** — Daxa's `TaskInterface`, renamed. Resolves a pass's declared handles to this
  frame's backing at execute time.
- **`engine_context`** — build-time services handed to pass constructors.
- **`resource_allocator` + `descriptor_table`** — Layer 0. Physical allocation and bindings.
  **Closed to redesign.** The only changes are RESTORATIVE: delete the three fabricated entry points,
  return `bind_buffer`/`bind_image`/`write_null` to private, and make the per-image sampler
  CONFIGURABLE at creation (the allocator hardcodes one config today — see below).

### Naming: snake_case, everywhere this brief touches (user, 2026-08-06)

Types, functions and namespaces are `snake_case`. This is not a new convention — it is the one the
user already named the resource layer in (`string::gpu::image`, `pass_context`, `engine_context`).
The `String::`/`PascalCase` half is the drift; earlier drafts of this brief reproduced it.

- `String::FrameGraph` -> `string::frame_graph`, `CompiledFrame` -> `compiled_frame`,
  `ResourceUsage` -> `resource_usage`, `ResourceStateTracker` -> `resource_state_tracker`,
  `DescriptorTable` -> `descriptor_table`, `GeometryPass` -> `geometry_pass`, and so on.
- The `String::` namespace collapses into `string::`.
- **Boundary:** everything this brief rewrites. `string-ui`, `string-asset*` and `string-debug` are
  renamed when they are next touched, not here — a whole-repo rename inside the big bang would
  multiply the diff without testing anything. Same rule, applied in the same order as the work.
- **The one real cost, named:** snake_case types collide with the obvious variable name (`image
  image;`). Call sites use `auto` for locals; where a name is genuinely needed it is qualified. This
  is the standard-library convention and it survives there for the same reason.

### The app seam — the app owns the graph, the library owns the frame (user, 2026-08-06)

**Decided with the user.** The app constructs and owns a `frame_graph`, declares every pass into it
**once**, and compiles it **once**. Each frame it calls `renderer.render_frame(frame, dt)`.

```cpp
// startup — authored ONCE. No callback, no re-run.
string::frame_graph fg;
demo_state s(ctx);

auto hdr     = fg.use_persistent(s.hdr);
auto shadow  = fg.use_persistent(s.shadow_maps);
auto froxels = fg.buffer({ .name = "froxels", .viewport_scaled = true });

fg.pass("shadow.cascades")
  .depth(shadow)
  .toggle(cv_pass_shadow)                  // in-graph conditional, NOT a recompile trigger
  .raster([&s](string::pass_context& c) { s.shadow.draw(c); });

auto frame = fg.compile();

while (running) { input.pump(); s.tick(dt); renderer.render_frame(frame, dt); }
```

**The split: declarations are the app's, derivations are the library's.** The app owns the graph
object and everything declared in it — so a second graph (an offline bake, a capture) is just another
object, not a new engine concept. The library keeps only what is *derived from* the graph rather than
declared in it: swapchain acquire, out-of-date/resize handling, queue placement with its QFOT halves,
and present. That is 04e's rule one level up.

**Why there is no author callback.** An earlier draft of this brief proposed
`std::function<void(frame_graph&, engine_context&)>`, re-run on every recompile. That was scaffolding
holding up a piece of drift: our graph currently **re-authors on every recompile**, because a toggle
or a resize throws the declarations away and rebuilds them. `docs/taskgraph_reference.md` does not
work that way —

```cpp
struct TaskImage { ResourceID current_image;                 // "Runtime state - managed by TaskGraph"
                   void set_images(std::span<ResourceID>); };
void conditional(TaskGraphConditionalInfo const&);
```

— a persistent resource swaps its **backing** without touching the graph, and a toggle is a
**conditional inside the graph**. Neither invalidates the compile. Once recompile-on-toggle and
recompile-on-resize are deleted, nothing needs re-authoring and the callback has no job.

**What this depends on (a real requirement, not a free lunch):** transient sizes must be
**declarable** relative to the viewport (`.viewport_scaled`), not computed at author time from a
concrete extent. Otherwise resize forces a re-author and the callback returns. Resize then stays
derived: transient reallocation and persistent rebinding both happen as backing swaps, invisible to
the declarations.

**Correction to an earlier claim in this brief:** the `DEVICE_LOST` class is closed by deleting the
`pass` base class — one list instead of two — not by the seam choice. It dies under any of these
options.

### The graph's shape (user, 2026-08-06)

**A pass is data plus a recording callback — not an object with a lifecycle.** The graph has two
states, matching `docs/taskgraph_reference.md`:

```
BUILDING   frame_graph     — declare passes (data + callback) and resources
COMPILED   compiled_frame  — toposorted, barriers derived; execute against it
```

We currently have **four** concepts covering those two:

| ours | reference | fate |
|---|---|---|
| `frame_graph` — author + `compile()` | `TaskGraph` (BUILDING) | **keep** — this is the graph |
| `compiled_frame` + `graph_plan` | `ExecutableTaskGraph` (COMPILED) | **keep, merged** — `compiled_frame` already *contains* `graph_plan`; one thing under two names |
| `render_plan` -> `Setup{passes, author}` | *nothing* | **DELETE** — the app owns the graph and declares into it directly |
| **`pass` base class** (`update`/`resize`/`record`/`record_compute` virtuals) | `Task` = name + declarations + callback | **DELETE as a base class** |

**`Pass` is the concept that generates the others.** Because it is an object with a per-frame
lifecycle rather than data + a callback, something must drive `update()`/`resize()` every frame — so
the app hands over a list of pass objects *in addition to* authoring the graph. That is
`RenderPlan::Setup{passes, author}`: two parallel lists, nothing enforcing that they describe the
same set in the same order, and the ordering between them is what caused the froxel `DEVICE_LOST`.

Removing the base class collapses all of it:

- A pass becomes `fg.pass("name").reads(x).writes(y).raster([=](pass_context& c){ ... })`.
- Pass **state** does not disappear — it stops being a `Pass` subclass. `GeometryPass` becomes a
  plain app-owned object whose callbacks capture it. This is Daxa's model.
- `update()` becomes ordinary app code running before `execute()` — not a graph concept.
- `resize()` disappears: the graph owns viewport-sized resources, so passes have nothing to resize.
- `record_compute()` disappears: it existed only because one `Pass` needed two execution points,
  which is the same defect as one graph node having two lifetimes.

So `RenderPlan`, the virtual triad, the two parallel lists, and the one-node-two-lifetimes problem
are one defect with one fix.

### Deleted concepts (agent-invented, not the design)

- `ResourceRegistry` as a class owning all resources and wrapping `descriptor_table`.
- `Lifetime` as a 5-value enum. Three of its values (`Imported`, `PerFrame`, `Streamed`) were backing
  strategies, not lifetimes; only two concepts exist and they need no tag.
- `binding(image, slot)` on a registry — `descriptor_table` owns bindings.
- `descriptor_table::bind_storage_view(VkImageView)` / `unbind_storage_view(slot)`. It fabricates
  synthetic resource ids from a `static` counter at `0xF0000000` that never resets or frees, purely
  to give per-mip HiZ views their own slots. `descriptor_type::STORAGE_IMAGE` already exists, and
  **most callers pass a whole-image view** (gtao, probe GI, post, IBL DFG) where `bind(id,
  STORAGE_IMAGE)` works directly.

### `descriptor_table` — restore the intended API (user, 2026-08-06)

The intended API is on branch `rewrite-6` and is **not** a redesign — it is what the table already
was before drift:

```cpp
class DescriptorTable {
public:
    explicit DescriptorTable(VkDevice, ResourceAllocator&);
    void bind  (ResourceID, DescriptorType);
    void unbind(ResourceID, DescriptorType);
    auto get_binding_slot(ResourceID, DescriptorType) -> std::uint32_t;
    auto get_layout() const -> VkDescriptorSetLayout;
    auto get_set()    const -> VkDescriptorSet;
private:                                  // <- PRIVATE on rewrite-6
    void bind_buffer(ResourceID, VkDescriptorType, std::uint32_t);
    void bind_image (ResourceID, VkDescriptorType, std::uint32_t);
};
```

Five public functions. `bind()` was sufficient on its own because **`bind_image` reads the sampler
off the resource** — `image.sampler`, from the allocator.

**The drift, and its root cause.** `resource_allocator::create_image_sampler` creates a sampler for
**every** image — on rewrite-6 and today, byte-identically — so `allocated_image.sampler` is always
populated and `bind_image` always has something to write. But it hardcodes exactly **one**
configuration:

```
LINEAR / LINEAR / mip LINEAR, REPEAT x3, anisotropy ON, compare OFF
```

Passes need others — shadow PCF wants `NEAREST` + `CLAMP_TO_EDGE`, GTAO's half-res upsample wants
`LINEAR` + `CLAMP_TO_EDGE`, and so on. With no way to ask for a different one:

1. Eight files call `vkCreateSampler` themselves (gtao, shadow, probe GI, IBL, post, meshlet, texture
   streamer, UI).
2. Their sampler is not the one on the image, so `bind(id, TEXTURE)` writes the wrong descriptor.
3. So `update_texture(slot, view, sampler)` was added to force the right one in.
4. And because `bind()` returns `void`, `get_binding_slot()` must be called separately.

The gap is **sampler configurability**, not a missing field. (An earlier draft of this brief said
`image.sampler` was null — that was wrong; it is populated, just always with the default.)

**Restore:**

- **Let the caller specify the sampler configuration at image creation** — a sampler description on
  `image_info`, defaulted to today's config so every existing call site is unchanged. That is what
  makes `bind()` sufficient again, deletes all 18 `update_texture` call sites and the 8 hand-rolled
  `vkCreateSampler` blocks, and puts sampler lifetime back with the image that owns it.
- **`bind_buffer` / `bind_image` return to private** (they are public today — a regression).
- **`write_null` becomes private** — zero external callers.

**Delete (fabricated):** `update_texture`, `bind_storage_view`, `unbind_storage_view`, and the
`0xF0000000` synthetic-id counter.

**Keep exactly as-is:** `bind`, `unbind`, `get_binding_slot`, `get_layout`, `get_set`. No
`update_binding`, no `subresource` parameter, no automatic release — the intended API needs none of
them.

### Sub-resource views (the one genuine need behind that hack)

HiZ writes mip N+1 while sampling mip N; IBL writes cube faces/mips. That is the ONLY thing
`bind_storage_view` was needed for, and the intended API has no mechanism for it — which is why the
synthetic-id counter was invented rather than the gap being raised.

The fix that keeps `descriptor_table` untouched: **a sub-resource view is its own `resource_id`.**
The allocator creates the per-mip / per-face view as a resource; `bind(view_id, STORAGE_IMAGE)` then
works through the unmodified API. On the logical side this is Daxa's `TaskImageView` — a slice of a
`TaskImage` — which since `string::gpu::image` IS `TaskImage` means the slice belongs on it:

```cpp
auto pyramid = fg.image({ .name="hiz", .format=R32F, .mips=n });   // transient
fg.pass("hiz.build")
  .reads (pyramid.mip(i))
  .writes(pyramid.mip(i + 1))
  .compute([=](pass_context& ctx){ ctx.slot(pyramid.mip(i + 1)); });   // -> get_binding_slot(view_id)
```

The graph sees mips as distinct usages (so it derives the chain), the descriptor slot comes from the
resource's own record, and no synthetic ids exist.

**Per-slice STATE tracking, not just per-slice binding (user, 2026-08-06).** `ResourceStateTracker`
tracks state per *image* today. Where the HiZ pyramid needed different states per mip, a `level_count`
parameter was added that transitions *all* mips at once, justified in a comment by an assumption
nothing checks:

```cpp
// `level_count` = mip levels to transition ... the HiZ pyramid, which is uniform
// across mips at every pass boundary — intra-pass per-mip [transitions stay local]
```

Under (b) the tracker holds state per slice, the graph derives the mip chain, and both that parameter
and its assumption go away.

## Agent-convenience allowances being revoked

The existing briefs permit incorrect and incomplete behaviour in places, and the justification is
agent convenience rather than the user's design or simplicity. Named, so they are not silently
re-inherited:

| allowance | where | what it actually bought |
|---|---|---|
| "either become graph nodes or keep local barriers — **agent judgment**" | 04e M2 | a blanket exemption for any chain that was hard to split |
| "**What NOT to declare**: intra-pass scratch consumed inside one record hook" | 04e M5 authoring contract | made the exemption the documented norm |
| "KEPT (**documented exceptions**)" | 04e M2/M5 | a list of places the graph does not work, preserved by writing them down |
| "leave the GI/IBL atlases + shadow maps as local-class ... **keeps the brief bounded**" | 11 | bounded the brief, not the problem |

**This brief revokes the intra-pass exemption.** A chain of dependent dispatches over slices of one
resource is expressible once slices are first-class, so "intra-pass" stops being a category and
becomes what it always was: passes that were not declared.

### The test an exception must pass

**"Before the graph exists" is structural. "The graph cannot express it" is a defect.** Only the first
survives. Every exemption in the briefs today is the second wearing the first's clothes.

Hand-rolled barriers surviving today, each judged against that test:

| site | count | verdict |
|---|---|---|
| `resource_state.cpp` | 3 | **KEEP** — this *is* the derived-barrier emitter. Not an exception to the rule; the implementation of it. |
| `renderer.cpp` QFOT release/acquire | 2 | **KEEP** — 04e M5: "GENERATED per-resource from declared `async_usages`, not hand-placed". The executor emitting a derived cross-queue edge. |
| `renderer.cpp` present transition | 1 | **KEEP** — already goes through `resource_states_.transition(..., Access::Present, ...)`. Derived. |
| construction-time uploads (`composite_pass` LUT, initial scene upload) | ~5 | **KEEP** — these run in pass *constructors*. No frame graph exists yet, so there is nothing to declare to. Genuinely pre-graph. |
| **per-frame streaming uploads** (`transfer_batch`) | 7 | **REVOKE** — `renderer.cpp:643` flushes the batch **inside the frame**, and its own comment says the writes reach this frame's draws "via **submission order** on the shared queue". A same-frame write→read edge held together by submission order and a hand barrier. `Access::TransferRead`/`TransferWrite` already exist with correct scopes and **nothing declares them** — the design anticipated transfer passes and the implementation hand-rolled them. |
| **`ui_pass` glyph atlas upload** | 3 | **REVOKE** — runs *inside* `record_frame`; uploads the atlas, then the same pass samples it. Textbook producer/consumer. |
| **`renderer.cpp` depth-resolve transition** | 1 | **REVOKE** — the comment states the reason: "Direct barrier (**not the tracker's DepthWrite scope, which would name LATE_FRAGMENT_TESTS**) … so **the tracker need not track it**." The vocabulary could not express "a depth resolve writes at COLOR_ATTACHMENT_OUTPUT", so it was bypassed. Framework-opens derives resolve setup and covers this. |
| `probe_gi_component` | 9 | **REVOKE** — capture / collapse / relight are declarable stages |
| `ibl_component` | 8 | **REVOKE** — cubemap mip + prefilter chain |
| `gtao` | 4 | **REVOKE** — raw → denoise is a producer/consumer pair |
| `geometry_pass` + `geometry_pass_meshlet` | 7 | **REVOKE** — HiZ mip chain; draw-cull → expand-scan → fill |
| `post_pass` | 3 | **REVOKE** — bloom mip chain |

**~44 hand-rolled barriers become derived. ~11 survive**, each because it is the mechanism itself or
predates the graph — not because declaring it was inconvenient.

## End state

```cpp
// Persistent: owned outside the graph, usage managed by the graph.
auto shadow = fg.use_persistent(shadow_maps);        // 3-layer D32, owner allocates
auto hdr    = fg.use_persistent(hdr_target);

// Transient: graph owns the lifetime, aliasable.
auto worklists = fg.buffer({ .name="worklists", .bytes=n });

fg.pass("shadow.cascades")
  .depth(shadow)                                    // access + stage inferred; no masks
  .reads(worklists)
  .toggle(cv_pass_shadow)
  .raster([=](pass_context& ctx){ ctx.rec.draw_mesh_tasks_indirect_count(...); });

fg.pass("geometry.phase1")
  .reads(shadow)                                    // neutral fallback 1.0 when shadow is off
  .reads(froxels).reads(sh).reads(ao)
  .color(hdr).depth(depth)                          // load/store/resolve DERIVED
  .raster([=](pass_context& ctx){ ... });
```

Rules restored:

- **No `resource_id` in pass code.** Passes hold `image`/`buffer`; `pass_context` resolves.
- **No stage masks in pass code.** Access and stage derive from the declaration and the pass kind.
- **No hand-rolled `vkCmdPipelineBarrier2` in pass code at all.** Not "none crossing a pass boundary"
  — the intra-pass exemption is revoked (see above). The only survivors are the tracker itself, the
  upload path (outside the frame graph), and the framework halves the renderer generates from
  declarations.
- **Graceful degrade is the graph's job** (brief 11): a disabled producer does not skip consumers;
  optional reads bind a neutral fallback (black additive, **1.0 shadow/AO**, sky-SH for GI). Only
  `.requires()` transitively skips. The neutral value is declared with the resource.
- **Framework-opens render passes** (confirmed with user): the executor derives load/store/resolve
  from declarations + lifetimes and calls `vkCmdBeginRendering` itself.

## Scope

**In:**

1. **A pass IS its declaration.** Data + one recording callback, authored once. The same declaration
   drives the planner and the barrier tracker; `Pass::usages` as a separate hand-written channel and
   the `pass` base class both disappear. `frame_graph` (BUILDING) -> `compiled_frame` (COMPILED) ->
   execute is the whole lifecycle; `render_plan` is deleted and the app owns the graph directly.
1b. **Authored once, compiled once.** Toggles become in-graph conditionals and persistent resources
   swap their backing, so neither a toggle nor a resize re-authors or re-plans. Transient sizes are
   declared `viewport_scaled` rather than computed from a concrete extent.
2. **Declaration over logical handles**, with `string::gpu::image_view` slices for mips/layers, and
   **per-slice state tracking** in `ResourceStateTracker` so the graph derives sub-resource chains.
3. **Persistent / transient split** as defined above: `use_persistent(...)` for externally-owned,
   `fg.image()/fg.buffer()` for graph-owned. No enum.
4. **`pass_context` completed** — `extent`, binding resolution, and slot-correct resolution for
   ring-backed persistents (today `id(image)`/`view(image)` ignore `frame_slot`).
5. **Graceful degrade made real** — the executor binds neutral fallbacks; today it computes
   `fallback_reads` at compile and references them **zero** times at execute.
6. **Framework-opens** — derive attachment load/store/resolve; retire the hand-coded MSAA chain and
   the `DepthResolve` marker.
7. **Intra-pass dispatch chains become passes**: HiZ mip chain, draw-cull -> expand-scan -> fill,
   GTAO raw -> denoise, probe GI capture/collapse/relight, IBL cubemap + prefilter, bloom mips.
8. **Per-frame uploads become declared transfer passes** — streaming uploads and the UI glyph atlas.
   `Access::TransferRead`/`TransferWrite` already exist and are unused; this is the first declaration
   of them. Construction-time uploads stay outside the graph (nothing exists to declare to yet).
9. **Pass-owned resources become graph resources**: shadow maps, GTAO targets, HiZ depth + pyramid,
   probe GI atlases, IBL cubemaps/DFG/SH, worklists, SceneData/light/stats/froxel rings.

**Deletions** (the win metric):

- `render_plan` and its `Setup{passes, author}` two-list structure — the app owns the graph directly
- **re-authoring on recompile**: the enabled-set-signature recompile trigger, `graph_dirty_`, the
  resize re-plan, and `rebuild_execution_plan` as a per-change operation. Authored once, compiled
  once; toggles are in-graph conditionals, resize is a backing swap
- the `pass` base class as an OO type: `update`/`resize`/`record`/`record_compute` virtuals,
  `bind_color_source`, `enable_predicate`. A pass becomes declarations + one callback
- `Pass::usages` / `async_usages` as a hand-written channel; `PassSpec` as a separate concept
- ~112 hand-written stage masks
- `ResourceRegistry`, `Lifetime`
- `descriptor_table`: back to the rewrite-6 API. Deleted: `update_texture` (18 sites),
  `bind_storage_view`/`unbind_storage_view` (41 sites) and the `0xF0000000` counter.
  `bind_buffer`/`bind_image`/`write_null` return to private. `bind`/`unbind`/`get_binding_slot`/
  `get_layout`/`get_set` unchanged
- the eight hand-rolled `vkCreateSampler` sites, once the sampler is configurable at creation
- per-pass bindless slot vectors (`shadow_slots_`, `gtao_*_storage_slots_`, `probe_*_slot_`,
  `mip_storage_slots`)
- the three hand-rolled degrades (`cascade_count = 0`, `gtao_slot = 0xFFFFFFFF`, `probe_gi = 0`)
- the RAW/WAR order-dependence workaround and its "known limitation" tests
- `ResourceStateTracker::transition`'s `level_count` parameter and its unchecked "uniform across mips"
  assumption
- ~44 hand-rolled barriers (of ~55 total); the ~11 survivors are the tracker emitter, the QFOT halves,
  the present transition and construction-time uploads
- the per-frame-in-flight **shadow ring**: written and sampled in the same frame, so the graph derives
  the write-after-read edge. **~96 MB VRAM** (144 → 48 MB), plus the GTAO/pyramid rings.

**Out:**

- **Streaming POLICY** — `want`/`unwant`/`collect_garbage` and residency decisions stay with the
  existing path. Only the resulting per-frame UPLOAD is declared to the graph, never the decision to
  stream.
- **Construction-time uploads** — they run in pass constructors, before any graph exists.
- **Layer 4** scene/ECS.
- **Multithreaded recording** — 04e M1 puts it explicitly out of scope.

## Verification

Behaviour **changes** where a pass is toggled off — neutral fallbacks replace the bespoke degrades.
Parity therefore applies to the all-enabled configuration; toggles are verified against the degrade
policy, not against today's output.

- **AE=0, all passes enabled**: exterior, non-square 1600x900, fixed-dt orbit, lookdev (fixed dt),
  interior.
- **Scene-switch round trip** (`lookdev@100,sponza@200`) AE=0 — exercises resource teardown/re-init.
- **Sync validation clean**, static + motion, async on and off. Bindless reads are invisible to the
  layer, so this is necessary, not sufficient.
- **Per-toggle deltas** match the policy: shadows off → lit unshadowed (not black), GTAO off → no AO,
  GI off → sky-SH ambient, geometry off → sky + UI over a cleared target. Re-enabling → AE=0.
- **No recompile after startup.** Assert the graph compiles exactly once: toggling every pass and
  resizing the window repeatedly must not re-author or re-plan. A second compile is a gate failure,
  since the whole seam design rests on this.
- **GI**: full probe bake + sustained relight under sync validation; GI-on before the bake completes
  must equal GI-off.
- **VRAM before/after reported** (04e M3 discipline). Expect ~96 MB+ reclaimed.
- **`tools/complexity.sh --gate`** passes. This brief should *reduce* complexity; if it does not,
  that is a finding to report, not a baseline to re-record.
- **Hand-barrier grep gate** (04e M5 discipline): every surviving `vkCmdPipelineBarrier2` /
  `vku::transition_image` site outside `resource_state.cpp` must be one of the ~11 justified
  survivors. A new one is a gate failure, not a documented exception.
- 3 nix checks green; hot reload, freeze-cull and every CVar lever unaffected.
- **NEEDS USER VISUAL VERIFY**: flythrough (interior/exterior/lookdev) + window resize. Sync and
  aliasing bugs show as flicker/corruption under motion. Run ONE headless GPU capture at a time.

## Risks

- **Deleting the `Pass` base class touches every pass and the app wiring.** Pass state moves from
  "subclass of Pass" to "app-owned object captured by callbacks". Mechanically large, but it is what
  removes the two-list structure that caused the DEVICE_LOST — the fragility is in the current shape,
  not the target one.
- **Bindless slot ownership moves off the passes.** Touches every bindless index in every push
  constant. Largest mechanical change here, and the point of it — VUID-09600 stops being
  hand-avoided.
- **Transient aliasing** engages interval packing that has never run on real content (04e M3 found no
  aliasable pairs in the frame as it then stood). 04e's two rules apply: acquire-from-UNDEFINED at
  each handoff, synchronised against the previous tenant's last access; and per-slot lifetimes so
  in-flight frames never share a placement.
- **Authored-once rests on two things holding.** In-graph conditionals must cover every toggle
  (including a toggled pass that other passes read from — the degrade path), and every
  viewport-derived size must be declarable rather than computed. If either leaks, the recompile
  trigger comes back and the author callback with it. This is the assumption to test first, not last.
- **Framework-opens + late-latch acquire.** The swapchain image is latched mid-record; derived
  attachment setup must tolerate a persistent whose backing is latched after compile.
- **Uploads become graph passes.** The per-frame streaming path currently relies on submission order
  on a shared queue. Declaring it makes the edge explicit, but if a streamed upload is consumed the
  same frame it lands, the derived edge must be right or textures pop as garbage for a frame. This is
  the least-exercised of the revocations.
- **Shadow ring removal is a real sync change**, not a refactor. If the derived write-after-read edge
  is wrong it shows as shadow corruption under motion — which settled captures cannot catch. This is
  the specific thing to look for in the visual verify.

## DEVIATION RECORD — agent judgment calls made during implementation (2026-08-06)

Written at the user's instruction so a later cleanup pass can find this cruft without re-deriving it.
**Nothing in this section is a user decision.** Each entry is something the agent chose where the
design did not say, or where the agent went past what it said. Default assumption for a cleanup pass:
these are wrong until argued otherwise.

### A. Additions to the declared surface

| # | what | design said | what the agent did | how to judge it later |
|---|---|---|---|---|
| A1 | **`per_frame` bool on `transient_image_info` / `transient_buffer_info`** | persistent/transient "need **no enum**"; a ring is "a backing detail the owner chose" | added a per-declaration ring flag for GRAPH-OWNED transients | **The strongest drift candidate — this is `Lifetime` shaped.** The measured inventory proves only TWO rings are genuinely temporal (`hiz_depth_ring_`, post `readback_`). The stricter alternative is to DERIVE it: ring a transient iff some pass declares a cross-frame read of it. If that works, A1 should be deleted outright |
| A2 | **`size_fn bytes_for` on `transient_buffer_info`** | transient sizes must be "declarable relative to the viewport" | images got a clean `viewport_scale` float; buffers got a `std::function<VkDeviceSize(VkExtent2D)>` callback | a callback is the weakest form of "declared". Only the froxel buffer needs it. If its size can be expressed as (tile_size, bytes_per_tile), replace the callback with those two fields |
| A3 | **`pass_kind::transfer`** | brief 11 names `.raster()` / `.compute()` as the alternatives; scope item 8 adds transfer passes | added a third kind | probably correct, but it was not written down before the agent added it |
| A4 | **`pass_lane { main, async }` enum** | `.async()` fluent setter | added an enum as its storage | trivial; storage for an approved setter |
| A5 | **`subresource` struct** (resource_state.hpp) | per-slice state tracking | a second range type alongside `gpu::image_view` | two types now describe a slice. One of them should probably go |

### B. Additions to files outside the graph

| # | what | why | judge later |
|---|---|---|---|
| B1 | **`vku::ImageTransition::base_layer`** | the struct had `base_mip`/`level_count`/`layer_count` but no layer offset, so a single cube face could not be transitioned | completes an existing struct; low risk |
| B2 | **`allocated_image::is_view`** | `create_view` shares the source's image/allocation, so `destroy_resource` must not free them twice | a direct consequence of the approved `create_view`; low risk |
| B3 | **`compile(engine_context&, VkExtent2D)`** | the brief's snippet is a bare `fg.compile()`; allocation needs the device | the agent said it would record this and initially did not. Signature drift from the agreed snippet, however small |

### C. Smells the agent introduced that resemble what this brief deletes

| # | what | why it is a smell |
|---|---|---|
| C1 | **`execute_info` names the swapchain THREE ways** — `resource_id swapchain`, `VkImage swapchain_image`, `VkImageView swapchain_view` | this is the same multiplicity shape as the deleted `ResourceUsage{resource, buf, img}` + `resolve()`. It should be ONE late-latched backing the graph resolves |
| C2 | **`slot_for(gpu::buffer, ...)` ignores `pass_index`** and always returns a `STORAGE_BUFFER` slot | the image path derives the descriptor type from the pass's own declaration, which is the whole point. The buffer path does not. Inconsistent |

### D. INCOMPLETE — currently reads as done but is not

These are the dangerous ones: the code compiles-shaped and the API looks finished, but the behaviour
is absent. A reviewer skimming the header would not notice.

| # | what | state |
|---|---|---|
| D1 | Neutral fallbacks were never created — degrade was wired to nothing | **CLOSED.** `materialize()` now mints a 1x1 fallback for every image that is written by some pass AND optionally read by another (the degradable set), same format and sampler; `init_fallbacks()` clears each to its declared neutral once, on the first executed frame |
| D2 | MSAA resolve was never derived | **CLOSED, and derived rather than declared.** A pass that declares TWO writes of one kind where one image is multisampled and one is not has declared a resolve; `derive_groups()` pairs them by sample count. No `.resolves_to()` verb, no marker access — which is the argument for having deleted `Access::DepthResolve` |
| D3 | `states_.clear()` at the end of every frame | **CLOSED.** The per-frame clear is removed: a layout is a property of the image, not of the frame that last touched it. State now carries across the boundary, which is what makes the `hiz_depth_ring_` cross-frame read derivable instead of hand-barriered. Only `resize()` clears |
| D4 | **Nothing has been compiled or run.** | **STILL OPEN** — no line of the landed work is verified beyond reading |

### The pass pattern (step 12), for the record

Every pass converges on this shape. `composite_pass` is the worked exemplar.

```cpp
class thing_pass                                   // no base class; the graph never sees the type
{
public:
    thing_pass(String::engine_context& ctx, ...);  // owns its GPU state, nothing else's
    void tick(float dt);                           // ordinary app code, not a graph concept
    void declare(string::frame_graph& fg, gpu::image in, gpu::image out);
private:
    void record(string::pass_context& ctx, gpu::image in);   // resolves everything through ctx
};
```

**`declare()` is a judgment call worth naming.** The brief says the app declares its passes. Putting a
`declare()` on the pass keeps the declaration next to the state that knows what it touches, and the
app still decides whether and when to call it — but the strictly literal reading is that the app
writes every fluent chain itself, in one place. With ~30 declared passes after the chain splits, that
one place would be very large. If a later pass disagrees, `declare()` is trivially inlinable into the
app: it is a call, not a layer.

**What the pattern deletes at each site:** the `usages` vector, every `Access::` and stage mask, every
stored bindless slot, every externally-pushed slot (`bind_color_source`, `set_source`), `update()`,
`resize()`, `record_compute()`, `debug_name()`, and every hand-rolled sampler.

### Defects found in the graph BY the pass conversions (step 12)

Four real bugs in `frame_graph`, all mine, all surfaced by converting real passes against it. Worth
recording because each one would have been invisible until it produced a wrong image.

| # | defect | consequence | fix |
|---|---|---|---|
| G1 | **`main_stage()` was read before the pass kind was known.** `.reads()`/`.writes()` resolved the stage from `building_.kind`, which the terminal `.compute()` sets afterwards | EVERY compute pass in the engine declared `FRAGMENT_SHADER` instead of `COMPUTE_SHADER`. Every derived barrier on a compute pass would have had the wrong stage | a default-stage use now records stage 0 meaning "this pass's main stage", resolved at the terminal where the kind is finally known |
| G2 | **`slot_for` matched by image, not by slice.** | a pass reading `mip(m-1)` and writing `mip(m)` got ONE descriptor type for both — a storage write resolving to a sampled descriptor. This is the whole bloom and HiZ chain | match the full `image_view`, falling back to an image-level match only when no slice matches |
| G3 | **Degrade applied to PERSISTENT resources.** A skipped producer substituted the neutral fallback regardless of lifetime | the UI glyph atlas's upload pass is toggled off on every frame the atlas is not dirty — nearly all of them — so `ctx.slot(atlas)` would have returned a 1x1 fallback and ALL TEXT would have disappeared | degrade is now transient-only. A transient's contents are frame-scoped, so "producer skipped" genuinely means "nothing in it"; a persistent's contents survive, so it means "not updated". The distinction is the whole justification for the feature |
| G4 | **Construction-uploaded persistents were discarded on first touch.** The tracker starts at `UNDEFINED`, so the first transition sourced from `UNDEFINED` — a discard — over a texture the transfer batch had already filled | streamed and construction-uploaded textures would sample as garbage | `persistent_image_info::initial_layout`, seeded into the tracker at compile. The graph cannot know this; the owner can |

| G5 | **`open_group` sized the render area from the FRAME viewport.** `renderArea`, viewport and scissor all came from `execute_info::extent` | a 2048x2048 shadow cascade group would rasterise only the top-left window-sized corner of its map. Shadows would be broken everywhere outside it | derive the extent from the group's own attachment image; the swapchain stays on the frame extent because its backing is late-latched |
| G6 | **Viewport-scaled transients truncated instead of rounding up.** `width * 0.5f` vs every consumer's `(width + 1) / 2` | at any odd viewport dimension a half-res target is one texel short of the dispatch grid writing into it — an out-of-bounds storage write every frame | round up |

G3 is the one to remember. It is not a coding slip — it was a wrong rule, and it was wrong in the
direction of *looking* correct: the feature worked, on the wrong set of resources. G5 is the one that
would have been blamed on the shadow rewrite rather than on the graph.

**All six were found by converting real passes against the graph, not by reading it.** That is worth
weighing against any future temptation to build graph machinery ahead of the code that uses it.

### The last stage masks, removed rather than excused

The shadow and transparency conversions needed `access::indirect_read` at `DRAW_INDIRECT` and a
task-stage storage read, so they reached for the explicit `.reads(buf, access, stage)` overload —
leaving two `VK_PIPELINE_STAGE_2_*` masks in pass code. That is a small deviation with a real reason,
and it was the shape of every exemption this brief revokes.

Removed instead: an access that can only happen at one stage now IMPLIES it. `indirect_read` is
always `DRAW_INDIRECT`, `index_read` always `INDEX_INPUT`, `vertex_read` always
`VERTEX_ATTRIBUTE_INPUT`, transfers always `COPY`. The stage parameter defaults to 0, so
`.reads(buf, access::indirect_read)` is now the whole declaration. Pass code names WHAT it does; the
graph knows where that happens.

**Still genuinely ambiguous, and therefore still explicit:** a storage read at the TASK stage. The
same access legitimately occurs at task, mesh, vertex, fragment or compute, so the access cannot
imply it and the pass has to say. That is the category the brief always allowed.

### G7 — CLOSED. Framework-opens now covers MRT, multiview and slice attachments

All four gaps fixed in `frame_graph.cpp`:

- **MRT**: `render_group::colors` is a vector. `attachment_of` returns every declared target and only
  collapses to a resolve pair when it finds EXACTLY one multisampled and one single-sample write.
  Anything else is MRT and all of it is bound. The old `found[0]` silently dropped attachments.
- **Multiview**: `render_group::view_mask` is DERIVED from the declared slice — a 6-layer colour slice
  is a cubemap capture, so the mask is `(1 << layers) - 1` and `layerCount` goes to 0 (they are
  mutually exclusive). Nothing declares a view mask; the slice already said it.
- **Slice views as attachments**: `open_group` binds `resolve_view(slice)` instead of the image's
  default view, so a cube image is bound as its 2D_ARRAY slice rather than as an (invalid) CUBE
  render target.
- **Per-attachment clears**: the clear value is the resource's declared `neutral`. That field already
  means "the value that cancels", which is exactly what an attachment should clear to — the probe
  capture's normal-depth target gets its `{0,0,0,-1}` sky-miss sentinel for free.

**The second gap is closed too**: `pass_context::slot(view, access)` resolves one image under two
descriptor types, disambiguated by the access the pass already declared. Probe GI's relight now uses
it for its read-modify-write, replacing the `.mip(0)` slice trick.

### The original G7 report, for the record

`compiled_frame::open_group` handles exactly one colour attachment, `viewMask = 0`,
`layerCount = 1`, and binds `allocated_image::view`. The probe-GI cube capture needs all four things
it lacks:

1. **MRT** — it writes `cube_albedo` AND `cube_nd`. `derive_groups`'s `attachment_of` currently takes
   `found[0]` when two colour writes have equal sample counts, so the second is silently DROPPED.
2. **Multiview** — `viewMask = 0x3F`, all six faces in one pass.
3. **Slice views as attachments** — it declares 6-layer 2D_ARRAY slices, but `open_group` binds the
   image's default view, which for a cube image is the CUBE view and is not a valid render target.
   It must resolve the declared slice through `resolve_view` instead.
4. **Per-attachment clear values** — `albedo` clears to `{0,0,0,0}` and `nd` to `{0,0,0,-1}` (the
   sky-miss sentinel). `open_group` hardcodes one clear.

**This also exposes an ambiguity in the MSAA-resolve derivation I added for D2.** "Two colour writes,
one multisampled and one not" means a resolve; "two colour writes of the same sample count" means
MRT. The rule reads sample counts, so it does not mis-fire — but it silently discards the MRT case
instead of supporting it, which is the worse failure because it looks like it worked.

Until this lands, `gi.capture` is declared correctly and renders wrong. It is declared honestly with
no hand-opened render pass, so the fix is entirely in `frame_graph.cpp` and nothing in the pass has
to change.

### A second `pass_context` gap

A pass cannot ask for one image under TWO descriptor types. `gi.relight`'s hysteresis reads the
irradiance atlas as a Sampler2D and writes it as an RWTexture2D in the same dispatch, and `slot_for`
returns the first matching use's type. Worked around by declaring the write as `.mip(0)` (an
identical subresource on a 1-mip image) so the two uses match distinct slices. That works and derives
correctly, but it is a trick, not a design. The clean fix is `pass_context::slot(view, access)`.

### REGRESSION (G8) — texture streaming lost its residency rebinding

The audit classified `update_texture` as pure drift, on the grounds that the real gap was sampler
CONFIGURABILITY. That was right for seven of its eighteen call sites and **wrong for the texture
streamer**, which used it for something else entirely:

- swapping a slot between the PLACEHOLDER view and the real image as the first mip lands;
- raising and lowering an adjustable **minLod** as the resident mip range grows and shrinks.

Neither is sampler configuration. Both are per-slot REBINDING of a live descriptor as residency
changes, and nothing in the restored five-function API replaces them. `bind()` writes the image's own
view and sampler, which is correct once a texture is fully resident and wrong while it is streaming.

The call sites now call `bind()` and are marked `BRIEF 20 REGRESSION` in `texture_streamer.cpp`.
**Textures will pop in at full resolution instead of refining**, and a scene unloaded mid-stream no
longer reverts to the placeholder.

This needs a design answer before the gates mean anything, and it should NOT be answered by
reinstating `update_texture` reflexively — the streamer was built on the drift, so "put it back" would
restore the hack along with the capability. The honest options are a `min_lod` on `sampler_info` plus
a way to rebind a resource's descriptor when its backing changes, or making a streamed texture's
resident range a first-class resource property the table reads. Deliberately left open.

### A real cost the design imposes: no sub-buffer ranges

Images have subresources; buffers do not. All six meshlet work lists (camera, twosided, and one per
shadow cascade) are REGIONS of a single frame-scratch buffer, so to the graph they are one resource —
every cull and expand pass now serialises against every other, even where the regions are provably
disjoint. Barrier *count* is roughly unchanged (~23 before, ~23 after), but the camera and cascade
chains no longer overlap.

This is a genuine loss, honestly stated rather than buried: the hand-rolled version was more parallel
than the derived one. Recovering it needs sub-buffer ranges in the declaration — the buffer analogue
of `image_view` — which this brief does not have and should not grow on the way past. Worth measuring
before deciding: if the cull chain is not on the critical path, the serialisation costs nothing real.

### Findings from the pass conversions (step 12)

**Three passes were relying on an attachment they never declared.** `grid_2d_pass`, `shadertoy_pass`
and `ui_background_pass` all call `enable_depth_stencil()`, so their pipelines carry a depth format
and require a depth attachment for dynamic-rendering compatibility — but none of them declared a
depth usage. Under the old renderer this worked by accident: the group's depth came from the
renderer's global depth target regardless of what any pass said. Under the graph, a group's depth
comes from a pass's own declaration, so the omission becomes visible immediately. Fixed by declaring
`.depth_read()` — deliberately NOT `.depth()`, because a write would make each of them the first
writer of the attachment and introduce a spurious clear.

This is the mechanism working as intended: an undeclared dependency that was silently satisfied by a
global is now either declared or broken. Expect more of these as the remaining passes convert.

**`engine_context::sample_count` is gone, and it should be.** Passes need the MSAA sample count to
build their pipelines, and it used to be a renderer-supplied service. But under this brief the
application declares the scene's colour and depth attachments, so the application is what chooses the
sample count — the renderer no longer has an opinion to supply. It is a constructor parameter on the
passes that need it, alongside the colour format, which is the same shape `composite_pass` already
had. Recorded because it is a seam that moved, not a field that vanished.

### E. Added during the renderer rewrite (step 10)

| # | what | why | judge later |
|---|---|---|---|
| E1 | **`renderer::capture_source(compiled_frame&, gpu::image)`** | the headless gates capture the resolved HDR target, which is now an APP-declared transient — the renderer cannot name it. The app states it once | a setter is a small seam, but it is a seam. The alternative is the app owning the capture entirely and the renderer exposing nothing, which would also move the `r.capture.*` CVar handling out of the frame loop |
| E2 | **`compiled_frame::note_external_layout()` + `resource_state_tracker::set_layout()`** | the debug capture drains the device and transitions its source directly, so the tracker's belief goes stale and the next frame derives a wrong barrier | this is a NOTIFICATION into the tracker, and notifications are how `seed()` started. It is narrower (layout only, no scopes, no sync derived from it) and it exists for a debug path rather than a render path — but if it ever gains a second caller, that is the signal it has become `seed()` again |
| E3 | **`compiled_frame::physical_of()`** | the capture path needs a real image id | a public resolution escape hatch. Pass code must never use it; if it ever does, that is drift |

### A deviation that was avoided rather than recorded

Closing D2 first added `persistent_image_info::samples` — the owner declaring a sample count the
graph needed. That was drift: the allocator already records `mip_levels` and `array_layers` on
`allocated_image` and simply did not record `samples`. It does now, `samples_of()` reads it from the
backing, and the declared field was deleted. **No new declaration surface.** Recorded here because
the reflex to add a field rather than fix the layer below it is the habit this brief exists to
correct.

## Measured inventory (2026-08-06) — corrections to the verdict table above

The verdict table was written from a grep. A full site-by-site audit corrects it. **51 distinct
barrier sites**, not ~55; the difference is lambda invocations counted twice.

| brief said | actual | note |
|---|---|---|
| `ibl_component` 8 | **9** | 7 in-frame + 2 debug-readback (`:427`, `:443`) behind `vkDeviceWaitIdle` on their own submit — those two are pre-graph survivors, not revokes |
| `gtao` 4 | **3** | and `gtao.cpp:212` is a CROSS-FRAME WAR, not the raw->denoise pair |
| `post_pass` 3 | **2 sites / 6 emissions** | all six are unscoped global `VkMemoryBarrier2` — the cleanest per-mip win |
| `ui_pass` 3 | **2** | |
| *(absent)* | **`geometry_pass.cpp:1947`** | the `hz.depth` DEPTH_ATTACHMENT restore — see below |

The grep also missed **5 `command_recorder::barrier` sites** (a raw `vkCmdPipelineBarrier2` at
`command_recorder.hpp:72`, distinct from `vku::transition_image`). Note `transition_image` hardcodes
`VK_QUEUE_FAMILY_IGNORED`, so it can never express a QFOT.

### RESOLVED — the "one exception" did not survive after all

The audit below concluded `hiz_depth_ring_`'s two hand-rolled barriers pass the test, because a
one-frame-late cross-slot layout lifecycle is not something an intra-frame graph models. That was
right about the dependency and wrong about the conclusion, and the reason is worth keeping.

Those barriers existed because **the tracker forgot everything at the frame boundary**. A resource
consumed one frame late therefore had to be hand-parked in a resting layout, because nothing
remembered what layout it was actually in. Once tracked state carries across the boundary — which it
now does, per slot, against that slot's own `VkImage` — the cross-frame edge derives exactly like any
other. `geometry_pass.cpp:1947` and its counterpart are both deleted.

So the count of hand-rolled barriers that survive on merit inside the frame is **zero**. The
survivors are the tracker itself, the QFOT halves the executor generates, and construction-time
uploads that genuinely predate the graph. "The graph cannot express it" turned out to be false even
in the one case that looked structural — it was a limitation of the tracker's memory, not of the
model.

### The exception as originally assessed

**`hiz_depth_ring_` is the only genuine cross-frame IMAGE dependency in the renderer.** GTAO
reprojection reads the PREVIOUS frame slot's resolved depth (`gtao.cpp:99-110` computes `prev_slot`,
`:185` reads `depth_history_[prev]`). `geometry_pass.cpp:1947` and `renderer.cpp:1437` are the two
halves of that cross-slot, one-frame-late layout lifecycle. This passes the test — it is not "the
graph cannot express it" wearing structural clothes, it is a real temporal dependency — but it does
NOT fall out of per-slice tracking, and must be designed for explicitly rather than assumed.

### Ring verdicts (measured, with proof)

| ring | genuinely temporal? | survives |
|---|---|---|
| shadow `images_[frame][cascade]` | **no** — every consumer indexes `current_frame`; no `prev` anywhere | **no** — 144 -> 48 MB, the **96 MB** confirmed |
| gtao `final_ring_` | no | no |
| hiz `hiz_pyramid_ring_` | no | no |
| **hiz `hiz_depth_ring_`** | **YES** | **YES** |
| post `readback_[f]` | **YES** — CPU readback, 3 frames late | **YES** |
| scene/light/stats/froxel, transparency `buffers_[f]` | no — host-write vs GPU-read pacing | no |

**Temporal but NOT ringed — in-place accumulators.** `probe_irrad_` (relight does a hysteresis EMA
reading `irrad_prev_slot` and writing `irrad_dst_slot` — the SAME image through two views, plus a
neighbour-bounce term over probes last written on an earlier frame; only 128 probes update per
frame), `visbits_buffer_`, `prev_draw_lod_buffer_`. These need a "reads prior contents AND writes"
declaration on ONE logical resource — a different shape from a ping-pong, and the thing
`probe_gi_component.cpp:689` currently holds together by hand.

**Pre-existing defect found, not caused by this brief:** `transfer_batch.cpp:277` uses `UNDEFINED` +
`TOP_OF_PIPE/0` as its source for a streamed mip subrange, which gives NO WAR execution dependency
against in-flight fragment reads of an already-resident texture. Safety rests entirely on caller
residency discipline. Its sibling barriers (`:234`, `:246`, `:314`) name `FRAGMENT_SHADER` only, so
compute/task/mesh samplers are covered incidentally by the `ALL_COMMANDS` global at `:124`. This is
the specific line that makes streaming "the least-exercised revocation".

**Chains where sub-resource views buy nothing** (declare them honestly, but expect no barrier
change): probe GI captures all 6 cube faces in ONE multiview pass and consumes them through a
SamplerCube — there is no per-face sequencing, and per-probe granularity is a TILE in a 2D atlas,
which Vulkan cannot express as a subresource. The IBL prefilter ladder already has no inner barriers.
The real per-mip wins are the HiZ reduction, the IBL capture mip chain, and the post bloom chain.

**Already fully derived — use as the template:** `shadow_maps.cpp`, `sorted_transparency.cpp`,
`froxel_component.cpp`, `sky_component.cpp`, `geometry_pass_lighting.cpp` contain ZERO barriers.

## Implementation order + landed state (2026-08-06)

One landing, no intermediate gates. The tree does NOT compile from step 2 until step 9 — that is
expected, not a regression to chase. Steps are ordered so each one's dependencies already exist.

### LANDED

1. **`string-core/include/string/gpu/resource.hpp`** — `sampler_info` (defaults are the allocator's
   previously hardcoded values, literally, so every existing sampler is an identical
   `VkSamplerCreateInfo`); `sampler` field on `image_info`; `gpu::image` / `gpu::buffer` /
   `gpu::image_view` with `.mip()` / `.mips()` / `.layer()` / `.whole()`; `allocated_image::is_view`.
2. **`resource_allocator`** — `create_view(source, view_range)` registers a sub-resource view as its
   own `resource_id` sharing the source's image/allocation; `destroy_resource` destroys only the view
   for one. `create_image_sampler` now reads `image_info::sampler`.
3. **`descriptor_table`** — restored to the five-function rewrite-6 API. `update_texture`,
   `bind_storage_view`, `unbind_storage_view`, `slot_to_synthetic_` and the `0xF0000000` counter are
   gone; `bind_buffer`/`bind_image`/`write_null` are private again.
4. **`resource_usage.hpp`** — snake_case `access` / `scope_of` / `resource_use` over logical handles.
   `DepthResolve`, the `resource`/`buf`/`img` multiplicity, `resolve()` and `key()` are gone.
5. **`resource_state.{hpp,cpp}`** — `resource_state_tracker`, tracking PER SUBRESOURCE (mip x layer)
   with run-coalescing so a whole-image transition still emits one barrier. `seed()`, `is_tracked()`
   and `level_count` are gone. `vku::ImageTransition` gained `base_layer`.
6. **`frame_graph.hpp` + `pass_context.hpp`** — the full declaration surface: `frame_graph`
   (BUILDING) -> `compiled_frame` (COMPILED), pass-as-declaration, persistent/transient,
   `viewport_scaled`, `set_images`, in-graph conditionals, `render_group`, `execute_info`.
7. **`frame_graph.cpp`** — authoring, resource declaration, `materialize`, subresource-aware
   collision test, Kahn toposort with authoring-order tiebreak, `derive_groups` (clear/load/store
   derived from attachment lifetime), `resize` as a backing swap, `compute_survivors` (conditionals
   + transitive `.requires()` skip), physical resolution.

8. **`compiled_frame::execute`** — the executor: survivor computation, barriers derived per pass from
   its declarations, render groups opened with derived load/store/resolve, async placement, present.
9. **`pass_context` resolution** — `id`/`view`/`address`/`mapped`, sub-resource views created on
   demand and cached, and `slot()` deriving the descriptor type from the pass's own declaration.
10. **`renderer`** — 1851 lines down to acquire, pacing, lane submit, present, resize. No passes, no
    graph, no render targets: the scene attachments are viewport-scaled transients the app declares.
12a. **Passes converted** (11 of ~17): `composite` (the exemplar), `grid_2d`, `shadertoy`,
    `ui_background`, `debug_line`, `ui` (+ its glyph-atlas transfer pass), `post` (11 declared
    passes), `shadow_maps` (**ring deleted — the 96 MB**), `sorted_transparency`, `gtao` (2 passes),
    `sky`, `froxel`, `ibl` (**14 declared passes**). HiZ is one declared pass per mip.

**Hand-rolled barriers remaining in every converted file: ZERO**, except the two inside
`ibl_component::run_verification`, which sit behind `vkDeviceWaitIdle` on their own submit and are
pre-graph by the brief's own test.

**Stage masks remaining in declarations: TWO**, both `storage_read @ TASK_SHADER` — the one case an
access genuinely cannot imply, since the same access happens at task, mesh, vertex, fragment and
compute.

11. **Deleted**: `render_pass.hpp`, `render_plan.hpp`, `resource_registry.{hpp,cpp}`,
    `graph_plan.{hpp,cpp}` and its test/example, plus their meson entries. The nine wrapper `Pass`
    subclasses in `geometry_pass.hpp` (SkyPass, FroxelPass, IblPass, GtaoPass, GiPass, ShadowPass,
    HizBuildPass, GeometryPhase2Pass, TransparencyPass) are gone — every one existed only to hold a
    per-frame `usages` vector and an `enable_predicate`.
12b. **`geometry_pass`**: no base class, `tick()`/`declare()`/`record_phase1`/`record_cull`/
    `record_phase2`, the construction-time AND per-frame `usages` vectors deleted, the
    `Access::DepthResolve` late-append deleted. `hiz_extent()`/`hiz_mip_count()` are pure functions of
    the viewport so the app and the pass agree on the pyramid's shape at author time.
14a. **`demo_scene`**: `declare_resources()` written — every scene resource declared as a logical
    handle with its viewport RELATIONSHIP rather than a computed size.

### IT RENDERS — real geometry, clean validation (2026-08-07)

```
[graph] compiled 84 passes, 11 groups, 8693 KiB transients
scene loaded: 1 files, 2049137 vertices, 405 draws, 28 materials, 72 textures, 79406 meshlets
validation errors: 0     device lost: 0     gtests: 77/77
```

Sponza draws through the rewritten graph — correct silhouettes, correct depth, sky behind the
geometry — with **zero validation errors and no device lost**. Repro:

```
SDL_VIDEODRIVER=offscreen STRING_RESOURCES_DIR=<stitched res dir> \
STRING_SCENE=sponza/main/NewSponza_Main_glTF_003.gltf \
STRING_CAPTURE_FRAME=60 STRING_CAPTURE_PATH=/tmp/spz.png ./build-b20/string_demo
```

Last defects on the way there: the MSAA resolve never fired because no pass declared BOTH a
multisampled and a single-sample colour write (so nothing reached the HDR target — a black frame);
the per-frame scratch arena was never materialised after the registry was deleted; the worklist
handle named a fresh graph buffer instead of the scratch arena the passes reserve their offsets from;
`ResetPush::stats` was left as a literal 0 when the registry was stubbed out, which is a null write
from a shader — GPUVM fault, device lost, exactly the crash class this brief exists to remove; and
`tick()` dereferenced the IBL component before the scene published it.

**Still open before the gates mean anything:** the parity battery has NOT been run (AE=0 exterior /
non-square / fixed-dt orbit / lookdev / interior, scene-switch round trip, per-toggle degrade deltas,
VRAM before/after, the hand-barrier grep gate, `tools/complexity.sh --gate`). Scene switching is
deliberately unwired (see above). G8 (texture-streaming residency rebinding) is an open regression.
The two placeholder sizes — the worklist arena's 1 MB and `kMaxLights` at 1024 — are still literals.

### PROCESS FAILURE — work destroyed by `git checkout` on an uncommitted tree (2026-08-07)

The meshlet conversion in `geometry_pass_meshlet.cpp` (~600 lines: `declare_cull`, `declare_expand`,
six `record_*` bodies on `pass_context`, four hand-rolled barriers deleted) was **destroyed** by an
agent running `git checkout <file>` to undo a bad edit. Nothing in this brief is committed, so that
is a delete, not an undo. No backup existed; it was fully re-done from the header signatures and this
brief's record of what the conversion produced.

**Two lessons, both cheap:**
1. On an uncommitted tree, `git checkout` / `git restore` / `git stash` are DELETE commands. Copy the
   file first (`cp x /tmp/x.bak`) — which had already been done twice for `frame_graph.cpp` in the
   same session, so the hazard was known and simply not applied here.
2. The reason a one-file slip became a permanent loss is that a change of this size ran for a whole
   session with **zero commits**. The user's standing rule is that they own commits; that rule is
   fine, but it means a WIP checkpoint has to be ASKED FOR at milestones (first clean build, first
   rendered frame), not skipped. Recovery cost here was an agent re-run; next time it could be the
   whole brief.

### RUN STATE — frames now execute (2026-08-07)

**Frames render.** The blocker was mine: a dangling `if (SceneRegistry::instance().has_pending())`
whose body I had commented out, which swallowed the next statement and skipped `render_frame` every
frame. Run headless with `SDL_VIDEODRIVER=offscreen`.

Fixed, in order, each found by running:

| # | defect | symptom |
|---|---|---|
| G9 | `create_view` shares the source's `VmaAllocation`; the allocator's DESTRUCTOR freed every image unconditionally (`destroy_resource` already honoured `is_view`, the dtor did not) | double free, segfault inside VMA |
| G10 | app-owned passes outlived the renderer that owns their allocator | teardown crash. `~Application` now drops the scene BEFORE the renderer, and says why |
| G11 | **`ctx.slot()` called `bind()` DURING recording**, invalidating the non-UPDATE_AFTER_BIND set | **182 recording-state errors.** Fixed by `bind_declared_slots()` at compile — the graph knows the whole declared set up front, which is exactly what makes lazy binding unnecessary |
| G12 | `physical()` indexed out of bounds on an invalid handle | segfault in compile |
| G13 | GI resource FIELDS were declared in `scene_resources` but never created with `fg.image()` | probe GI declared invalid handles |

**Score: 182 validation errors + segfault -> 4 errors, no crash.**

Two more, found after the above:

| # | defect | symptom |
|---|---|---|
| G14 | **`bind_declared_slots` chose the descriptor type from the ACCESS alone.** A buffer declared with `transfer_write` fell through to TEXTURE, so the table looked a buffer id up in the image map and threw `out_of_range` mid-construction — which unwound destructors and destroyed images while the upload command buffer was still recording. The "three destroyed images" were a SYMPTOM of that throw, not a cause | crash + 4 recording-state errors |
| G15 | **The access-qualified `slot_for` overload still called `bind()`.** I stripped the bind from the one-argument overload and added the two-argument one afterwards, reintroducing exactly the bug I had just fixed | 206 recording-state errors |

The descriptor family now follows the resource KIND (an image is never a storage buffer), with the
access only refining it within images; and attachment/transfer uses are skipped entirely, since a
colour write is not something a shader indexes.

**A FRAME RENDERS.** `STRING_CAPTURE_FRAME=5` writes a PNG. It is BLACK, and 93 validation errors
remain — but they are now barrier-SCOPE complaints (`dstAccessMask` not supported by `dstStageMask`,
image-layout mismatches, a queue-submit expecting a different layout), i.e. **the graph deriving
wrong barriers, not corrupting the command buffer.** That is a different and much better class of
problem: the machinery works, the derivation has bugs.

**Score across the session: does not compile -> compiles -> links -> 77/77 tests -> runs -> renders a
frame. 182 recording-state errors -> 0. Remaining: 93 barrier-scope errors and a black image.**

Three more fixed after that, taking 93 -> 65 -> 47 -> **41**:

| # | defect | fix |
|---|---|---|
| G16 | `scene.upload` was declared a TRANSFER pass, so its reads defaulted to the COPY stage — and a sampled read is illegal there | it records no GPU commands at all, so its kind was always nominal; compute is the legal one |
| G17 | the resolve-target transition hardcoded `COLOR_ATTACHMENT_OUTPUT` for depth as well as colour | depth resolves complete at LATE_FRAGMENT_TESTS; the depth access flags are not even legal at the colour stage |
| G18 | the app hardcoded the swapchain format as `B8G8R8A8_UNORM`; the real one is SRGB, so composite's pipeline never matched its attachment | `renderer::swapchain_format()` exposes the real one. A guessed format is a validation error at first draw and nothing before it |

| G19 | **the async lane's command buffer was submitted to the GRAPHICS queue.** Its pool was created for queue family 1 | submit to `submission_lanes()[lane].vk_queue`. Beyond being invalid, it made every layout the async work established a lie to the main lane |

**HYPOTHESIS TESTED AND FALSIFIED.** The obvious explanation was the cross-frame state carry (D3)
meeting a skipped pass. It is NOT: adding `states_.clear()` back at the end of every frame leaves the
count at **exactly 35**. The carry is innocent, and so is the whole skipped-pass line of reasoning —
**the mismatch happens WITHIN a single frame.** Do not re-litigate D3; the experiment is one line and
it has already been run.

**THE TWO OFFENDING RESOURCES ARE NAMED** (dump the record->VkImage map in `bind_declared_slots` to
reproduce): `VkImage 0xdd` is **`ibl.env_capture`** and `0xda` is **`shadow.cascade2`**.

`ibl.env_capture` is the informative one, and the complaint is per-level: *every* mip of layer 0,
"cannot transition from GENERAL when the previous known layout is SHADER_READ_ONLY". That image is
declared BOTH ways in the same chain — `ibl.capture.mip{m}` reads mip m-1 and writes mip m as
STORAGE (GENERAL), while each `ibl.prefilter.{m}` reads the WHOLE cube SAMPLED (SHADER_READ_ONLY).
So a whole-image sampled read and per-slice storage writes disagree about the layout of the same
cells, and the tracker's belief and the barrier's coverage come apart.

**A tracker trace was taken and it EXONERATES the tracker.** Instrumenting
`resource_state_tracker::transition` for that VkImage (an env-var-gated log of
`mip/layer/old->want/write`) shows 114 transitions that are all internally consistent: writes take
cells to GENERAL(1), the whole-cube sampled reads take them to SHADER_READ_ONLY(5), and run-
coalescing behaves — the log emits one line per coalesced run, which is why only the run-leading mips
appear. Belief and coverage agree at every step.

So the remaining mismatch is NOT the tracker's bookkeeping and NOT the whole-image-vs-slice
declaration per se. What is left is ORDER: the sequence the tracker records must differ from the
sequence the GPU executes. The next thing to check is the toposort's placement of
`ibl.prefilter.{m}` relative to `ibl.capture.mip{m}` — both touch `env_capture`, one whole-image and
one per-slice, and `collides()` compares subresource ranges, so a whole-image read and a mip write DO
overlap and must produce an edge. If that edge is missing or inverted, the barriers are recorded in
one order and executed in another, which is exactly this symptom.

**This is the first defect that is a design question rather than a slip**, which is why it is the
right place to stop: mixing a whole-image view and per-slice views of one image is exactly what
sub-resource declarations were introduced to allow, and the tracker has to have a coherent answer for
it. Two candidate answers, neither yet tested: (a) a whole-image use must expand to every slice the
image has been sliced into, so coverage and belief always match; (b) declaring both forms on one
image in one chain is illegal and should be rejected at compile with a clear message.

One fix already landed from this line of investigation and should be kept regardless: the UI glyph
atlas is uploaded by the transfer batch during construction (leaving it SHADER_READ_ONLY) and was
registered with no `initial_layout`, so the graph's first barrier asserted a layout nobody had told
it about. It now declares `initial_layout`. That is exactly what G4 added the field for, and nothing
had used it.

**REMAINING: 35 errors, two clusters, both the SAME root shape** — the tracker believes a layout the
image is not actually in:
- `expects VkImage ... DEPTH_STENCIL_ATTACHMENT_OPTIMAL, current layout is UNDEFINED`
- `cannot transition from GENERAL when the previous known layout is SHADER_READ_ONLY_OPTIMAL`

**REMAINING: (earlier count, superseded) 41 errors, dominated by one cluster (15 + 9).** A depth image is expected in
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` at submit but is actually `UNDEFINED` — the tracker believes a
layout the image never reached. The suspicion to test first: this is the cross-frame state carry (D3)
meeting a SKIPPED pass. Tracked state now persists across frames, so frame N+1 records barriers
assuming the layout frame N left behind — but if the pass that would have established that layout was
toggled off (or its group never opened, e.g. shadow cascades with no resident geometry), the layout
was never actually reached and the assumption is false.

If that is confirmed, the fix is principled rather than a patch: **a skipped pass must not leave the
tracker asserting state its work would have produced.** `compute_survivors()` already knows which
passes did not run; the tracker needs to be told to forget what those passes would have written,
rather than inheriting a belief nobody established.

Two hypotheses tested and ELIMINATED, so the next person does not repeat them:
- *Stale handles from lazy view creation* — no. `bind_declared_slots()` already calls `resolve_view`
  for every declared slice at compile, so the allocator's map is not mutated during a frame.
- *The swapchain sentinel reaching `get_image()`* — fixed (the swapchain now barriers through
  `swapchain_image_` directly), and the error count did not move.

So three images are GENUINELY destroyed between compile and the first frame. The suspects are the
passes that call `destroy_resource` OUTSIDE their destructor — i.e. that recreate their own backing
after the graph has already latched a physical id for it:

- `composite_pass::bake_and_upload_lut` (`:310`) retires and re-creates `lut_image_` when the LUT
  size changes; the first `tick()` is a bake.
- `ui_pass` (`:402-412`) retires its per-frame ring buffers.
- `texture_streamer` (`:80`, `:87`).

**This is a DESIGN hole, not just a bug.** A persistent whose owner recreates its backing must
re-point the handle with `set_images()`, and nothing enforces or even detects that today — the graph
goes on using an id the allocator has freed. Whatever fixes the immediate crash should also make the
omission impossible or loud: either the graph owns those images too, or `destroy_resource` on an
id the graph holds is an error.

### BUILD STATE (2026-08-06)

**It compiles, links and starts.** `meson compile -C build-b20` is clean across all eight libraries
and the sandbox; `string-core`'s gtest suite is **77/77 green**, including 12 new `frame_graph` tests
covering stage resolution, subresource identity, live conditionals and declaration bookkeeping.

At startup the graph reports:

```
[graph] compiled 84 passes, 11 groups, 33504 KiB transients
```

84 declared passes where there were ~17 pass objects, 11 derived render groups, and the transient
arena the graph now owns outright.

**NOT YET VERIFIED, and an earlier claim of mine was wrong.** I reported "zero sync hazards over 100
seconds". That was worthless: the window receives a CLOSE event immediately in this environment, so
`render_frame` is never called and **not one frame has executed**. Everything up to and including
compile runs; nothing past it has. A clean validation log with zero frames rendered proves nothing,
and I should not have offered it as evidence.

So the true state is: **built, linked, unit-tested, and started — never rendered.** Every gate in
the Verification section is still outstanding, and they need a real display session.

One defect was found and fixed by getting this far: `render_frame` began a command buffer without
resetting its pool, because I deleted a `submissions_.reset()` call when that method turned out not
to exist and never replaced it. `command_recorder::reset()` is the real one, and it is now called per
slot before recording — safe because the timeline wait in `begin_frame` IS the guarantee that the
slot's GPU work has completed.

### REMAINING, in order

8. **`compiled_frame::execute`** — the executor. Walk `order_`; for each surviving pass derive
   barriers from its declared uses against `states_`; open/close render groups (framework-opens:
   `vkCmdBeginRendering` with derived load/store/resolve); place `pass_lane::async` passes on
   `execute_info::async_rec` when present, else inline. Emit the present transition. Bind neutral
   fallbacks for optional reads whose producer did not survive.
9. **`pass_context` resolution** (out-of-line in `frame_graph.cpp`) — `id`/`view`/`address`/`mapped`,
   and `slot()` deriving the descriptor type from the pass's own declaration for that handle, and
   returning the FALLBACK's slot when the producer was skipped. Mint the 1x1 neutral fallback images
   during compile via the existing construction-time upload path.
10. **`renderer.{hpp,cpp}`** — delete `record_frame`, the group loop, `open_group_rendering`,
    `image_of`/`image_view_of`/`image_meta`, `rebuild_execution_plan`, `graph_dirty_`,
    `enabled_signature_`, `scene_passes_`/`frame_passes_`, `bind_composite_source`. What remains is
    acquire, resize, `render_frame(compiled_frame&, dt)`, submit with the lane timelines, present.
11. **Delete** `render_pass.hpp`, `render_plan.hpp`, `resource_registry.{hpp,cpp}`, `graph_plan.*`;
    drop `resources` from `engine_context`.
12. **Passes** — each becomes a plain app-owned object (no base class) whose callbacks capture it.
    Order: composite, grid2d, shadertoy, ui_background, debug_line (trivial) -> ui, post (bloom mip
    chain splits) -> sky, froxel, ibl (cubemap mip + prefilter split) -> gtao (raw/denoise split) ->
    shadow -> transparency -> probe GI (capture/collapse/relight split) -> geometry (HiZ mip chain
    and draw-cull -> expand-scan -> fill split; two-phase is already three passes).
    Each pass's owned images/buffers move to graph resources; the shadow ring collapses.
13. **Per-frame uploads** become declared transfer passes (streaming + UI glyph atlas).
14. **`demo_scene.cpp`** — app owns `frame_graph`, declares once, compiles once, calls
    `render_frame`.
14a2. **`Application`** now owns the graph: `initialize(info, scene_fn)` constructs the scene, authors
    once, compiles once, publishes introspection; the loop is `tick(dt)` then
    `render_frame(frame, dt)`. `RenderPlan` is gone from the engine seam entirely.

**SCENE SWITCHING IS DELIBERATELY UNWIRED.** It used to rebuild a `RenderPlan` and hand it to
`Renderer::load_scene`. With the app owning the graph, a switch means tearing down the pass objects
and authoring a fresh `frame_graph` — which is app work, not renderer work. Left as a TODO in
`application.cpp` rather than half-wired: a scene switch that silently kept stale declarations is
exactly the class of bug this brief exists to remove. Its gate (`STRING_SCENE_SWITCH`) will fail
until this is done, and that is the correct signal.

14b. **`demo_scene` pass wiring** — the three `RenderPlan::Setup` lambdas (demo_scene.cpp ~line 569
    onward) still build the old pass list. Replace with: construct the pass objects, call each
    `declare()` against the handles from `declare_resources()`, `fg.compile(ctx, extent)` ONCE, then a
    frame loop of `tick()` calls followed by `renderer.render_frame(frame, dt)`. `sandbox/
    render_types.hpp` and `application.{hpp,cpp}` still name the deleted types.
15. **snake_case sweep** across `string-core` + `string-render-forward` (`String::` -> `string::`).
    Mechanical; do it LAST so it does not churn under the real edits.
16. **Build, then the full verification battery** in one pass.

**Pass-declaration order matters and is NOT free.** The toposort breaks ties by authoring index, so
declare in the intended execution order: froxel/ibl/gtao/gi producers, shadow, geometry.phase1, the
HiZ chain, geometry.phase2, transparency, debug lines, UI, post, composite. This is the ordering the
old `Setup.passes` push order encoded implicitly — the difference is that a wrong order now shows up
as a wrong derived barrier rather than a frame-0 null address.

Measured at the start: ~15-18k lines of change; 127 `Pass` sites, 354 `String::` sites, 135
registry/descriptor sites, 49 hand-rolled barriers, ~12.4k lines of pass implementation.

## Full API audit vs `rewrite-6` (2026-08-06)

Everything that will survive this brief, diffed against the pre-drift baseline. Additions are guilty
until justified.

### `Pass` (`render_pass.hpp`)

| addition | verdict |
|---|---|
| `debug_name()` | **KEEP** — brief 11 M4 introspection |
| `enable_predicate` + `is_enabled()` | **KEEP as mechanism** — brief 11 `.toggle()`. But it must be *set* by the fluent `.toggle()`, not read independently by the renderer; two owners of the same fact is how the two-channel split started |
| `async_usages` + `record_async_compute()` | **KEEP** — 04e M4 |
| `resize()` made virtual | **KEEP** — passes size their own targets |
| **`record_compute()`** | **DELETE** — a *second* record entry point on one Pass. This is what makes a pass "one node with two execution points", which is why `geometry.phase1`'s graph edges are coincidentally rather than causally correct. Brief 11 specifies `.raster(...)` / `.compute(...)` as **alternative** bodies. A pass that needs both is two passes. |
| **`bind_color_source(slot, physical_id)`** | **DELETE** — the renderer *pushes* the HDR target's bindless slot into `PostProcessPass` at init and on resize. The design is that post declares `.reads(hdr)` and *asks* `pass_context`. This is the hand-passed-slot class brief 16 says the design eliminates — the same shape as the froxel `DEVICE_LOST`. |

### `Access` (`resource_usage.hpp`)

| addition | verdict |
|---|---|
| `IndirectRead` | **KEEP** — 04e M2 names it explicitly |
| `StorageImageRead` / `StorageImageWrite` | **KEEP** — a real Vulkan distinction (GENERAL layout vs buffer), brief 09 |
| **`DepthResolve`** | **DELETE** — its own comment says "a MARKER usage … NOT a graph write". An enum value that is not an access, invented to replace the `depth_resolve_target()` hook. Framework-opens derives resolve setup and removes the need. |

### `ResourceUsage`

Was `{resource, access, stage}`. Now carries **three** ways to name a resource (`resource`, `buf`,
`img`) plus `resolve()` to pick between them, plus `key()` partitioning synthetic id bands at
`1<<50` / `1<<51`. **DELETE the multiplicity** — the same fabricated-id pattern as the descriptor
table's `0xF0000000`. One logical handle (or view), resolved at execute.

### `ResourceStateTracker`

| addition | verdict |
|---|---|
| `buffer_access()` + `flush_buffers()` | **KEEP** — 04e M2: buffers tracked by id, hazards merged per flush point |
| **`seed()` + `is_tracked()`** | **DELETE** — added so the tracker could be *told* about the MSAA resolve write it could not observe. Framework-opens makes the executor own the resolve, so it becomes observable and the back door closes. |
| **`level_count` on `transition()`** | **DELETE** — per-slice state tracking replaces it (see above) |

### `resource_allocator` / `image_info`

| | verdict |
|---|---|
| allocator public API | **UNCHANGED vs rewrite-6** — clean |
| `image_info` gained `mip_levels`, `samples`, `cube` | **KEEP** — real feature needs (mipped images, MSAA, cubemaps), not workarounds |
| sampler configuration | **ADD** — the one genuine gap; see above |

### `descriptor_table`

Covered above: restore the rewrite-6 API, delete the three fabricated entry points, re-privatise
`bind_buffer`/`bind_image`/`write_null`.

### Not audited (flagged, not cleared)

`FrameGraph`/`GraphPlan` internals beyond the declaration surface; `command_recorder` verbs;
`submission_set`/lane machinery; `presenter`; the UI, asset and debug libraries. Brief 11's
introspection (M4) and light/material store — which briefs 12–15 build on — are **unaudited** and
should be assumed to need the same treatment.
