#pragma once

// Brief 20 — the render graph. Two states, one declaration channel.
//
//   BUILDING   frame_graph      declare passes (data + a recording callback) and resources
//   COMPILED   compiled_frame   toposorted, transients packed, groups derived; execute against it
//
// A pass IS its declaration: a name, what it touches, and one callback. There is no pass base class,
// no second hand-written usage channel, no resource_id in pass code and no pipeline-stage masks in
// pass code. The graph derives ordering, barriers, layouts, attachment load/store/resolve and queue
// placement from what was declared — that is the whole point of declaring.
//
// AUTHORED ONCE, COMPILED ONCE. A toggle is an in-graph conditional evaluated at execute; a resize
// swaps resource BACKING and re-sizes viewport-scaled transients. Neither re-authors and neither
// re-plans, so there is no recompile trigger and nothing needs a re-runnable author callback.
//
// The app owns the graph object and every declaration in it; the library owns what is DERIVED from
// the graph rather than declared in it — swapchain acquire, resize, queue placement and present.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace String { struct engine_context; }

namespace string
{

class frame_graph;
class compiled_frame;
struct pass_context;

// The pass body. One callback: the pass records its work through the context, which is also how it
// resolves every handle it declared. There is no second entry point — a pass that needs two is two
// passes, which is exactly what makes its edges derivable.
using record_fn = std::function<void(pass_context&)>;
// Re-evaluated at EXECUTE, every frame. Empty => always enabled. This is the in-graph conditional:
// flipping it does not recompile anything.
using enable_fn = std::function<bool()>;
// Recomputes a viewport-derived size. Declared, not computed at author time — that is what lets a
// resize be a backing swap instead of a re-author.
using size_fn = std::function<VkDeviceSize(VkExtent2D)>;

enum class pass_kind : std::uint8_t { raster, compute, transfer };

// Where the graph may place a pass. `async` marks a dependency-free compute chain the scheduler puts
// on an async compute lane when the hardware exposes one; on 1-lane hardware it records inline on the
// main queue — same graph, serialized placement, no special cases.
enum class pass_lane : std::uint8_t { main, async };

// --- resource declarations ----------------------------------------------------------------------

// An externally-owned image: the owner allocates and destroys it, the graph manages its USAGE only.
// One physical id, or one per frame-in-flight (a ring) — the graph resolves by slot either way, so a
// ring is not a distinct lifetime, just a backing detail the owner chose.
struct persistent_image_info
{
    std::string name;
    std::vector<gpu::resource_id> physical;
    // Late-latched: the backing is not known until the frame acquires it (the reference doc's
    // TaskImage::swapchain_image). The executor substitutes the acquired image at execute.
    bool swapchain = false;
    // The layout the owner's backing is ALREADY in when the graph first sees it. The graph cannot
    // know this: a texture uploaded during construction is left in SHADER_READ_ONLY by a transfer
    // that ran before any graph existed, and a tracker starting from UNDEFINED would transition
    // with a discard and throw those contents away. Default UNDEFINED means "nothing in it yet",
    // which is right for a target the graph will fully write before anything reads it.
    VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // What a consumer binds when this resource's producer is toggled off (graceful degrade). The
    // neutral value is declared WITH the resource because only the resource knows what cancels:
    // 1.0 for shadow/AO, black for an additive term.
    VkClearColorValue neutral{};
};

struct persistent_buffer_info
{
    std::string name;
    std::vector<gpu::resource_id> physical;
};

// A frame-scoped image the graph allocates, owns and may alias against other transients whose
// lifetimes do not overlap.
struct transient_image_info
{
    std::string name;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{ 1, 1, 1 };
    // Sized from the viewport rather than fixed: extent = viewport * scale, recomputed on resize
    // without re-authoring. Declaring the RELATIONSHIP is what keeps the graph authored-once.
    bool viewport_scaled = false;
    float viewport_scale = 1.0f;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    bool cube = false;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    gpu::sampler_info sampler{};
    VkClearColorValue neutral{};
    // One physical per frame-in-flight. Needed only when a resource is read one frame after it is
    // written (temporal reprojection); everything else is single-buffered and the graph derives the
    // within-frame edge instead of spending memory on it.
    bool per_frame = false;
};

struct transient_buffer_info
{
    std::string name;
    VkDeviceSize bytes = 0;
    // Declared viewport relationship (see transient_image_info::viewport_scaled). When set, `bytes`
    // is recomputed by this function on resize.
    size_fn bytes_for;
    VkBufferUsageFlags usage = 0;
    VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VmaAllocationCreateFlags allocation_flags = 0;
    bool per_frame = false;
};

// --- pass declaration ---------------------------------------------------------------------------

struct pass_decl
{
    std::string name;
    pass_kind kind = pass_kind::raster;
    pass_lane lane = pass_lane::main;
    std::vector<resource_use> uses;      // reads and writes; is_write(u.how) distinguishes them
    std::vector<bool> required;          // parallel to `uses`: a .requires() read transitively skips
    enable_fn enabled;                   // empty => always on
    record_fn record;
};

// Fluent per-pass builder. The terminal raster()/compute()/transfer() sets the kind and the body and
// appends the pass to the graph.
class pass_spec
{
    frame_graph& graph_;
    pass_decl building_;

    static VkPipelineStageFlags2 main_stage(pass_kind kind);
    // Fill any use declared without an explicit stage with the pass's main stage. Runs at the
    // terminal, because that is the first moment the pass KIND is known.
    void resolve_stages();
    pass_spec& add(const resource_use& u, bool required);

public:
    pass_spec(frame_graph& g, std::string name) : graph_(g) { building_.name = std::move(name); }

    // --- reads. Optional by default: if the producer is toggled off the pass still runs and binds
    //     the resource's declared neutral fallback. Only .requires() propagates the skip. ----------
    pass_spec& reads(gpu::image_view v);
    pass_spec& reads(gpu::image i)  { return reads(i.whole()); }
    pass_spec& reads(gpu::buffer b);
    pass_spec& reads(gpu::image_view v, access how, VkPipelineStageFlags2 stage = 0);
    pass_spec& reads(gpu::image i, access how, VkPipelineStageFlags2 stage = 0) { return reads(i.whole(), how, stage); }
    pass_spec& reads(gpu::buffer b, access how, VkPipelineStageFlags2 stage = 0);

    pass_spec& requires_(gpu::image_view v);
    pass_spec& requires_(gpu::image i) { return requires_(i.whole()); }
    pass_spec& requires_(gpu::buffer b);

    // --- writes -----------------------------------------------------------------------------------
    pass_spec& writes(gpu::image_view v);
    pass_spec& writes(gpu::image i) { return writes(i.whole()); }
    pass_spec& writes(gpu::buffer b);
    pass_spec& writes(gpu::image_view v, access how, VkPipelineStageFlags2 stage = 0);
    pass_spec& writes(gpu::image i, access how, VkPipelineStageFlags2 stage = 0) { return writes(i.whole(), how, stage); }
    pass_spec& writes(gpu::buffer b, access how, VkPipelineStageFlags2 stage = 0);

    // --- attachments. Load/store/resolve are DERIVED from the graph's lifetime facts, never
    //     declared: the first writer clears, later writers load, the last before a resolve
    //     resolves. ----------------------------------------------------------------------------
    pass_spec& color(gpu::image_view v);
    pass_spec& color(gpu::image i) { return color(i.whole()); }
    pass_spec& depth(gpu::image_view v);
    pass_spec& depth(gpu::image i) { return depth(i.whole()); }
    pass_spec& depth_read(gpu::image_view v);
    pass_spec& depth_read(gpu::image i) { return depth_read(i.whole()); }

    // --- gating + placement -----------------------------------------------------------------------
    pass_spec& toggle(enable_fn pred) { building_.enabled = std::move(pred); return *this; }
    pass_spec& toggle(const bool* flag) { building_.enabled = [flag] { return *flag; }; return *this; }
    pass_spec& async() { building_.lane = pass_lane::async; return *this; }

    // --- terminal ---------------------------------------------------------------------------------
    void raster(record_fn fn);
    void compute(record_fn fn);
    void transfer(record_fn fn);
};

// Read-only view of an authored pass, for tooling. Taken BEFORE the conditional is applied, so every
// pass is listed with its live enabled state — including the ones currently off.
struct pass_info
{
    std::string_view name;
    pass_kind kind;
    pass_lane lane;
    bool enabled;
};

// --- the graph (BUILDING) -----------------------------------------------------------------------

class frame_graph
{
public:
    frame_graph() = default;
    frame_graph(const frame_graph&) = delete;
    frame_graph& operator=(const frame_graph&) = delete;

    // Externally-owned resources: the graph manages usage, never lifetime.
    gpu::image  use_persistent(const persistent_image_info& info);
    gpu::buffer use_persistent(const persistent_buffer_info& info);
    // Re-point a persistent handle at fresh backing (the reference doc's TaskImage::set_images).
    // This is how a resize reaches the graph: the declarations never change, only the backing.
    void set_images (gpu::image h,  std::span<const gpu::resource_id> physical);
    void set_buffers(gpu::buffer h, std::span<const gpu::resource_id> physical);

    // Graph-owned, frame-scoped, aliasable.
    gpu::image  image (const transient_image_info& info);
    gpu::buffer buffer(const transient_buffer_info& info);

    pass_spec pass(std::string name) { return pass_spec(*this, std::move(name)); }

    // Allocate transients, pack aliasable ones, toposort, derive render-pass groups. Called ONCE.
    compiled_frame compile(String::engine_context& ctx, VkExtent2D viewport);

    std::vector<pass_info> passes() const;

    // --- resource records (the graph is the single resolution authority) -------------------------
    struct image_record
    {
        std::string name;
        bool transient = false;
        bool swapchain = false;
        bool per_frame = false;
        VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::vector<gpu::resource_id> physical;   // 1, or one per frame slot
        transient_image_info info{};              // transient only (re-created on resize)
        VkClearColorValue neutral{};
        gpu::image fallback{};                    // 1x1 neutral substitute, minted at compile
    };

    struct buffer_record
    {
        std::string name;
        bool transient = false;
        bool per_frame = false;
        std::vector<gpu::resource_id> physical;
        transient_buffer_info info{};
    };

    const image_record&  record_of(gpu::image h)  const { return images_[h.index]; }
    const buffer_record& record_of(gpu::buffer h) const { return buffers_[h.index]; }
    const std::vector<pass_decl>& declarations() const { return passes_; }

private:
    friend class pass_spec;
    friend class compiled_frame;
    void add(pass_decl&& p) { passes_.push_back(std::move(p)); }

    std::vector<pass_decl> passes_;
    std::vector<image_record> images_;
    std::vector<buffer_record> buffers_;
};

// --- the compiled graph (COMPILED) ---------------------------------------------------------------

// A run of consecutive passes sharing one dynamic-rendering instance. Derived, not declared: passes
// that write the same color/depth attachments and sit adjacent in the toposort form one group, and
// the framework opens it (vkCmdBeginRendering) with load/store/resolve derived from the group's
// position in each attachment's lifetime.
struct render_group
{
    std::uint32_t first = 0;   // index into compiled_frame::order
    std::uint32_t count = 0;
    // Colour attachments, in declaration order. More than one is MRT — the probe-GI cube capture
    // writes albedo and normal-depth together. A single multisampled entry paired with a
    // single-sample one is a RESOLVE instead, which is why the two cases are distinguished by
    // sample count rather than by count alone.
    std::vector<gpu::image_view> colors;
    // All faces/layers of a layered attachment rendered in one pass (viewMask). Derived from the
    // declared slice: a 6-layer colour slice is a cubemap capture.
    std::uint32_t view_mask = 0;
    std::optional<gpu::image_view> color;
    std::optional<gpu::image_view> depth;
    // MSAA resolve targets, derived from the attachments' sample counts: a multisampled attachment
    // paired with a 1-sample image of the same size resolves into it at EndRendering.
    std::optional<gpu::image_view> color_resolve;
    std::optional<gpu::image_view> depth_resolve;
    bool clears_color = false;   // this group is the first writer of `color` this frame
    bool clears_depth = false;
    // Does any pass in the group WRITE depth? Read-only depth must be bound in the read-only layout —
    // binding it as a writable attachment contradicts the barrier its own declaration produced.
    bool depth_writes = false;
    bool last_color = false;     // last group writing `color` -> store/resolve rather than store
};

struct execute_info
{
    gpu::command_recorder& rec;
    gpu::command_recorder* async_rec = nullptr;   // null => async passes record inline on `rec`
    std::uint32_t frame_slot = 0;
    VkExtent2D extent{};
    // Late-latched swapchain backing for this frame (persistent_image_info::swapchain).
    gpu::resource_id swapchain = 0;
    VkImage swapchain_image = VK_NULL_HANDLE;
    VkImageView swapchain_view = VK_NULL_HANDLE;
};

class compiled_frame
{
public:
    compiled_frame() = default;

    // Run the frame. Evaluates conditionals, derives every barrier from the declarations against
    // tracked state, opens/closes render-pass groups, and places async passes on the async recorder
    // when one is supplied.
    void execute(const execute_info& info);

    // Re-size viewport-scaled transients and forget tracked state. Does NOT re-author or re-plan:
    // the declarations are unchanged, only the backing. Caller waits for device idle first.
    void resize(String::engine_context& ctx, VkExtent2D viewport);

    // Did the executor skip `pass_index` (order index) this frame? For introspection/timing.
    bool ran(std::uint32_t order_index) const;

    const std::vector<std::uint32_t>& order() const { return order_; }
    const std::vector<render_group>& groups() const { return groups_; }
    frame_graph* graph() const { return graph_; }

    // Total bytes the graph allocated for transients (reported after compile; the aliasing win).
    VkDeviceSize transient_bytes() const { return transient_bytes_; }

    // Resolve a handle's backing for a frame slot. Public for the debug capture path, which has to
    // reach a real image; pass code resolves through pass_context instead.
    gpu::resource_id physical_of(gpu::image h, std::uint32_t slot) const { return physical(h, slot); }
    gpu::resource_id physical_of(gpu::buffer h, std::uint32_t slot) const { return physical(h, slot); }

    // Tell the tracker an image's layout was changed OUTSIDE the graph. The debug capture drains the
    // device and transitions its source directly, so without this the tracker's belief goes stale and
    // the next frame's derived barrier is wrong. Announcing it is what keeps the tracker the single
    // authority even when a debug path has to step around it.
    void note_external_layout(gpu::image h, VkImageLayout layout);

private:
    friend class frame_graph;

    friend struct pass_context;

    // Allocate every transient from its declaration (viewport-scaled ones against `viewport`).
    void materialize(String::engine_context& ctx, VkExtent2D viewport);
    // Group adjacent raster passes sharing attachments, and derive each group's clear/load/store
    // facts from that attachment's lifetime across the frame.
    void derive_groups();
    // Seed the tracker with each persistent's already-established layout (see initial_layout).
    // Bind every declared resource into the bindless table once, at compile — never during recording.
    void bind_declared_slots();
    void seed_persistent_layouts();

    // Derive and emit every barrier a pass's declarations imply, against tracked state.
    void barrier_for(VkCommandBuffer cmd, const pass_decl& p, std::uint32_t slot,
                     bool skip_attachments);
    // Open a render-pass instance with load/store/resolve derived from the group's lifetime facts.
    void open_group(VkCommandBuffer cmd, const render_group& g, std::uint32_t slot, VkExtent2D extent);
    static VkImageAspectFlags aspect_of(VkFormat format);
    // One-shot clear of each neutral fallback to its declared value, on the first executed frame.
    void init_fallbacks(VkCommandBuffer cmd);
    VkSampleCountFlagBits samples_of(gpu::image h) const;
    // Is this resource produced by a pass that survived this frame's conditionals? Persistent
    // resources always count: their contents outlive the frame, so a skipped producer means "not
    // updated", not "invalid".
    bool producer_alive(gpu::image h) const;

    // Resolve a declared use to this frame's physical backing + subresource range.
    gpu::resource_id physical(gpu::image h, std::uint32_t slot) const;
    gpu::resource_id physical(gpu::buffer h, std::uint32_t slot) const;
    // A slice resolves to its own resource_id, created on demand and cached (allocator::create_view).
    gpu::resource_id resolve_view(gpu::image_view v, std::uint32_t slot) const;
    // The bindless slot for a handle THIS pass declared, with the descriptor type derived from that
    // declaration — and the neutral fallback substituted when the producer did not survive.
    std::uint32_t slot_for(gpu::image_view v, std::uint32_t pass_index, std::uint32_t slot) const;
    std::uint32_t slot_for(gpu::image_view v, access how, std::uint32_t pass_index, std::uint32_t slot) const;
    std::uint32_t slot_for(gpu::buffer h, std::uint32_t pass_index, std::uint32_t slot) const;

    gpu::resource_allocator& allocator() const;

    // Survivor computation for this frame's conditionals: a disabled pass drops, and a pass whose
    // .requires() read has no surviving producer drops with it (transitively). Optional reads of a
    // dropped producer bind the resource's neutral fallback instead.
    void compute_survivors();

    frame_graph* graph_ = nullptr;
    String::engine_context* ctx_ = nullptr;
    std::vector<std::uint32_t> order_;          // toposorted pass indices
    std::vector<render_group> groups_;
    std::vector<bool> alive_;                   // per declared pass, this frame
    resource_state_tracker states_;
    VkDeviceSize transient_bytes_ = 0;
    VkExtent2D viewport_{};
    // This frame's late-latched swapchain backing (see persistent_image_info::swapchain).
    gpu::resource_id swapchain_ = 0;
    VkImage swapchain_image_ = VK_NULL_HANDLE;
    VkImageView swapchain_view_ = VK_NULL_HANDLE;

    // Sub-resource views, created on first use and reused. Keyed by the slice plus the frame slot,
    // because a per-frame resource's slices belong to different backing images.
    struct slice_key
    {
        std::uint32_t image = 0, base_mip = 0, mip_count = 0, base_layer = 0, layer_count = 0, slot = 0;
        bool operator==(const slice_key&) const = default;
    };
    struct slice_hash
    {
        std::size_t operator()(const slice_key& k) const
        {
            std::size_t h = k.image;
            for (std::uint32_t v : { k.base_mip, k.mip_count, k.base_layer, k.layer_count, k.slot })
                h = h * 1099511628211u ^ v;
            return h;
        }
    };
    mutable std::unordered_map<slice_key, gpu::resource_id, slice_hash> slices_;
    // Fallbacks created by materialize(), awaiting their one-shot clear.
    std::vector<gpu::image> pending_fallbacks_;
};

}  // namespace string
