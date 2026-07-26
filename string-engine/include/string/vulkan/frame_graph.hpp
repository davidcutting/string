#pragma once

// Brief 11 — fluent render-graph authoring facade (the top-level API shape locked 2026-07-25).
//
// This sits ON TOP of the existing planner (render_graph.hpp: GraphBuilder/RenderGraph) and
// lowers to the SAME `ResourceUsage` vocabulary the executor already consumes (resource_usage.hpp)
// — one model, shared. It adds what the planner lacks and the brief-11 vision needs:
//
//   - Fluent Daxa-style authoring:  fg.pass("x").reads(a).writes(b).toggle(cv).compute(fn)
//   - Lean typed resource handles over resource_id — NO generation counter (brief 11 decision:
//     timeline semaphores already guard the critical GPU-lifetime sync; keep it lean).
//   - Per-pass enable/disable (.toggle) with the GRACEFUL-DEGRADE fallback policy: a disabled
//     producer does NOT skip its consumers — optional reads bind a neutral fallback resource
//     (cleared to a value that cancels), and ONLY reads marked .requires() transitively skip.
//     (This is what makes debug pass-toggling behave: shadows off -> lit scene, not black.)
//
// SCOPE — FIRST INCREMENT (this file): the authoring + compile layer (ordering, enable/disable,
// fallback classification), unit-tested WITHOUT a GPU. Deliberately out of scope here, tracked as
// follow-on milestones in docs/briefs/11-pipeline-modularity.md:
//   * resource *allocation* — image()/buffer() currently mint LOGICAL handles only; physical
//     backing wires to the allocator + FrameScratch later.
//   * *execution* — feeding CompiledFrame into the Renderer's frame loop (replacing the imperative
//     Pass::usages + record() hooks) and the persistent compile+invalidate plan.
//
// Naming: `FrameGraph` (author-facing fluent builder) is distinct from `RenderGraph` (the planner
// OUTPUT) and `GraphBuilder` (the low-level planner builder). Reconciling the three names is a
// follow-up once execution is wired.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/render_graph.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace String
{

// --- lean typed handles over resource_id (no generation; reuse the existing id space) ----------
enum class ResourceKind : std::uint8_t { Image, Buffer };

template <ResourceKind K>
struct Handle
{
    string::gpu::resource_id id = 0;
    constexpr bool valid() const { return id != 0; }
    friend constexpr bool operator==(Handle a, Handle b) { return a.id == b.id; }
};
using ImageHandle  = Handle<ResourceKind::Image>;
using BufferHandle = Handle<ResourceKind::Buffer>;

enum class PassKind : std::uint8_t { Raster, Compute };

// The pass body. Mirrors Pass::record's signature so the executor wiring (later milestone) is a
// thin adapter, not a re-plumb.
using RecordFn = std::function<void(string::gpu::command_recorder&, std::uint16_t)>;

// Toggle predicate: re-evaluated at compile time. Empty => always enabled. A CVar-bound bool is
// wrapped into one of these by the caller (the debug-UI checkbox writes the same CVar — brief 14).
using EnablePredicate = std::function<bool()>;

// --- internal per-pass record (authoring intent, pre-compile) -----------------------------------
struct FgRead
{
    string::gpu::resource_id resource;
    Access access;
    VkPipelineStageFlags2 stage;
    bool required;   // .requires() -> transitive skip when the producer is disabled; else fallback
};

struct FgWrite
{
    string::gpu::resource_id resource;
    Access access;
    VkPipelineStageFlags2 stage;
};

struct FgPass
{
    std::string name;
    PassKind kind = PassKind::Raster;
    std::vector<FgRead> reads;
    std::vector<FgWrite> writes;
    EnablePredicate enabled;   // empty => always on
    RecordFn record;
};

// --- compile output -----------------------------------------------------------------------------
struct CompiledPass
{
    std::string name;
    PassKind kind;
    // Lowered usages for the planner/executor. EXCLUDES fallback reads (they touch a neutral
    // substitute, not the named resource, so they must not forge ordering edges).
    std::vector<ResourceUsage> usages;
    // Optional reads whose producer was disabled: the executor binds the resource's neutral
    // fallback here instead of the real image/buffer (brief 11 graceful-degrade).
    std::vector<string::gpu::resource_id> fallback_reads;
    const RecordFn* record;   // points into the source FrameGraph (valid for the compile's lifetime)
};

struct CompiledFrame
{
    std::vector<CompiledPass> passes;   // in toposorted execution order
    RenderGraph plan;                   // planner output (adjacency / toposort / lifetimes)
};

class FrameGraph;

// Fluent per-pass builder. reads/writes/requires/toggle configure the pass; raster()/compute()
// finalize it (set kind + body) and append it to the graph.
class PassSpec
{
    FrameGraph& graph_;
    FgPass building_;

    static constexpr VkPipelineStageFlags2 main_stage(PassKind kind)
    {
        return kind == PassKind::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                         : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }

    void add_read(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s, bool required)
    {
        building_.reads.push_back({ r, a, s, required });
    }

public:
    PassSpec(FrameGraph& graph, std::string name) : graph_(graph)
    {
        building_.name = std::move(name);
    }

    // --- reads (optional by default: a disabled producer -> neutral fallback, pass still runs) ---
    // Typed convenience: an image read defaults to SampledRead, a buffer read to StorageRead, both
    // at the pass's main stage. Use the explicit .read(res, Access, stage) form for anything else
    // (vertex/index/indirect fetch, task/mesh-stage reads, storage-image reads).
    PassSpec& reads(ImageHandle h)  { add_read(h.id, Access::SampledRead, main_stage(building_.kind), false); return *this; }
    PassSpec& reads(BufferHandle h) { add_read(h.id, Access::StorageRead, main_stage(building_.kind), false); return *this; }
    PassSpec& read(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { add_read(r, a, s, false); return *this; }

    // --- requires (essential: a disabled producer transitively skips THIS pass too) --------------
    PassSpec& requires_(ImageHandle h)  { add_read(h.id, Access::SampledRead, main_stage(building_.kind), true); return *this; }
    PassSpec& requires_(BufferHandle h) { add_read(h.id, Access::StorageRead, main_stage(building_.kind), true); return *this; }
    PassSpec& require(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { add_read(r, a, s, true); return *this; }

    // --- writes ----------------------------------------------------------------------------------
    PassSpec& writes(ImageHandle h)  { building_.writes.push_back({ h.id, Access::StorageImageWrite, main_stage(building_.kind) }); return *this; }
    PassSpec& writes(BufferHandle h) { building_.writes.push_back({ h.id, Access::StorageWrite, main_stage(building_.kind) }); return *this; }
    PassSpec& write(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { building_.writes.push_back({ r, a, s }); return *this; }

    // --- attachments (raster) --------------------------------------------------------------------
    PassSpec& color(ImageHandle h) { building_.writes.push_back({ h.id, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT }); return *this; }
    PassSpec& depth(ImageHandle h) { building_.writes.push_back({ h.id, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT }); return *this; }

    // --- gating ----------------------------------------------------------------------------------
    PassSpec& toggle(EnablePredicate pred) { building_.enabled = std::move(pred); return *this; }
    PassSpec& toggle(const bool* flag) { building_.enabled = [flag] { return *flag; }; return *this; }

    // --- finalize (append to the graph). raster()/compute() are terminal. ------------------------
    void raster(RecordFn fn);
    void compute(RecordFn fn);
};

class FrameGraph
{
    std::vector<FgPass> passes_;
    // Transient logical ids are minted below the renderer's SWAPCHAIN/COLOR/DEPTH sentinels and
    // above 0 (allocator ids count up from 0). This space is for LOGICAL identity only until
    // physical allocation is wired (see file header).
    string::gpu::resource_id next_transient_ = TRANSIENT_BASE;

public:
    // Reserved logical-id band for graph transients (well below the renderer sentinels at the top
    // of the id space; comfortably above allocator-assigned ids in practice for the planner tests
    // and the future execution wiring, which will resolve these to physical backing).
    static constexpr string::gpu::resource_id TRANSIENT_BASE = (string::gpu::resource_id{1} << 48);

    // Mint a transient logical handle. (Descriptor/allocation is a follow-on milestone; `name` is
    // retained for the future introspection API.)
    ImageHandle image(const std::string& /*name*/)  { return ImageHandle{ next_transient_++ }; }
    BufferHandle buffer(const std::string& /*name*/) { return BufferHandle{ next_transient_++ }; }

    // Import an externally-owned resource by its existing id (e.g. COLOR_TARGET, a persistent
    // buffer, a streamed texture) — the graph treats it as an always-available input it never
    // produces.
    ImageHandle import_image(string::gpu::resource_id id)  { return ImageHandle{ id }; }
    BufferHandle import_buffer(string::gpu::resource_id id) { return BufferHandle{ id }; }

    PassSpec pass(const std::string& name) { return PassSpec(*this, name); }

    // Enable/disable + graceful-degrade + lowering + toposort. Pure; no GPU. See the .cpp-less
    // definition below (kept inline so this stays header-only for the first increment).
    CompiledFrame compile() const;

private:
    friend class PassSpec;
    void add(FgPass&& p) { passes_.push_back(std::move(p)); }
};

inline void PassSpec::raster(RecordFn fn)
{
    building_.kind = PassKind::Raster;
    building_.record = std::move(fn);
    graph_.add(std::move(building_));
}

inline void PassSpec::compute(RecordFn fn)
{
    building_.kind = PassKind::Compute;
    building_.record = std::move(fn);
    graph_.add(std::move(building_));
}

inline CompiledFrame FrameGraph::compile() const
{
    const auto n = static_cast<uint32_t>(passes_.size());

    auto is_enabled = [](const FgPass& p) { return !p.enabled || p.enabled(); };

    // A resource is EXTERNAL (an input, always available) iff NO pass in the graph writes it.
    // A resource written by some pass but by NO SURVIVING pass has a "disabled producer".
    std::unordered_map<string::gpu::resource_id, bool> written_by_any;
    for (const FgPass& p : passes_)
        for (const FgWrite& w : p.writes)
            written_by_any[w.resource] = true;

    // Survivor fixpoint: a pass is dropped if it is disabled, OR if any of its .requires() reads
    // names a resource that IS produced somewhere but NO surviving pass produces it (its producer
    // was disabled and there is no neutral fallback for an essential input). Iterate: dropping a
    // pass removes its writes, which may starve another pass's required read.
    std::vector<bool> survive(n);
    for (uint32_t i = 0; i < n; ++i) survive[i] = is_enabled(passes_[i]);

    auto produced_by_survivor = [&](string::gpu::resource_id r) {
        for (uint32_t i = 0; i < n; ++i)
            if (survive[i])
                for (const FgWrite& w : passes_[i].writes)
                    if (w.resource == r) return true;
        return false;
    };

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!survive[i]) continue;
            for (const FgRead& rd : passes_[i].reads)
            {
                if (!rd.required) continue;
                const bool external = written_by_any.find(rd.resource) == written_by_any.end();
                if (external) continue;                       // real input, always available
                if (!produced_by_survivor(rd.resource))       // producer disabled, no fallback
                {
                    survive[i] = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    // Lower surviving passes onto the existing planner. Fallback reads (optional, producer
    // disabled) are recorded separately and EXCLUDED from the planner usages so they forge no
    // ordering edge against a resource nothing produces this frame.
    GraphBuilder builder;
    std::vector<std::vector<string::gpu::resource_id>> fallback_reads(n);
    std::vector<uint32_t> lowered_index;   // planner pass index -> original pass index
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!survive[i]) continue;
        const FgPass& p = passes_[i];
        PassBuilder pb = builder.add_pass(p.name);
        for (const FgWrite& w : p.writes)
            pb.use(w.resource, w.access, w.stage);
        for (const FgRead& rd : p.reads)
        {
            const bool external = written_by_any.find(rd.resource) == written_by_any.end();
            if (!external && !produced_by_survivor(rd.resource))
            {
                // optional (required ones already caused a skip above) read of a disabled
                // producer -> neutral fallback, not a graph edge.
                fallback_reads[i].push_back(rd.resource);
                continue;
            }
            pb.use(rd.resource, rd.access, rd.stage);
        }
        pb.end_pass();
        lowered_index.push_back(i);
    }

    CompiledFrame frame;
    frame.plan = builder.build();

    frame.passes.reserve(frame.plan.toposorted.size());
    for (uint32_t planner_idx : frame.plan.toposorted)
    {
        const uint32_t orig = lowered_index[planner_idx];
        const FgPass& p = passes_[orig];
        CompiledPass cp;
        cp.name = p.name;
        cp.kind = p.kind;
        cp.usages = frame.plan.passes[planner_idx].usages;
        cp.fallback_reads = fallback_reads[orig];
        cp.record = &p.record;
        frame.passes.push_back(std::move(cp));
    }
    return frame;
}

}  // namespace String
