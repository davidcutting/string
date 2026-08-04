#pragma once

// Brief 11 — fluent render-graph authoring facade (the top-level API shape locked 2026-07-25).
//
// This sits ON TOP of the existing planner (graph_plan.hpp: PlanBuilder/GraphPlan) and
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
// Naming (reconciled, brief 11): ONE author-facing graph — `FrameGraph` (this file, the fluent
// builder + compiler). It lowers onto the internal planner in graph_plan.hpp: a `PlanBuilder`
// produces a `GraphPlan` (the plan = adjacency / toposort / lifetimes). So the vocabulary is
// FrameGraph (public) -> PlanBuilder -> GraphPlan (plan), with `CompiledFrame::plan` a GraphPlan.
// No competing "*Graph"/"*Builder" names remain.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/graph_plan.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <volk.h>

namespace String
{

// Brief 16 M7: the graph layer identifies resources by raw `resource_id` — its identity was always a
// resource_id, and the old phantom-typed `Handle<Image>/Handle<Buffer>` only wrapped one with a kind
// tag. That typing job now lives one layer up on the real logical handles (`string::gpu::image`/
// `buffer`, resource_registry.hpp); the graph doesn't need it. (`Handle<*>` retired.)

enum class PassKind : std::uint8_t { Raster, Compute };

// The pass body. Brief 16 M1: takes a pass_context& (the execute-time surface — command_recorder +
// registry + frame slot) instead of a bare (command_recorder&, frame). The executor builds one
// pass_context per invocation; an authoring adapter that wraps a legacy Pass unpacks ctx.rec /
// ctx.frame_slot, while a handle-native pass resolves its resources through ctx directly.
using RecordFn = std::function<void(string::gpu::pass_context&)>;

// Toggle predicate: re-evaluated at compile time. Empty => always enabled. A CVar-bound bool is
// wrapped into one of these by the caller (the debug-UI checkbox writes the same CVar — brief 14).
using EnablePredicate = std::function<bool()>;

// A prepass/compute body: returns true if it recorded work (mirrors Pass::record_compute).
using ComputeFn = std::function<bool(string::gpu::pass_context&)>;
// A dynamic yes/no query re-evaluated each frame (e.g. "does the async chain have work this frame?").
using QueryFn = std::function<bool()>;

// A getter the graph calls to read a pass's current logical usages. Brief 16: this REPLACES the old
// `usagesFrom(&pass->usages)` raw pointer into a pass's public member — the graph now reads usages
// through an interface the pass controls (returns a stable reference; the graph never holds a pointer
// into pass internals). The usages carry LOGICAL registry handles (ResourceUsage::buf/img); the
// executor resolves the per-frame physical via ResourceUsage::resolve() — resolution is the graph's
// single job, not the pass's.
using UsagesFn = std::function<const std::vector<ResourceUsage>&()>;

// The full RUNTIME surface of a pass — everything the executor needs to run it WITHOUT the Pass
// interface. Callbacks close over the (stable) pass object. This retires the Pass* `source` bridge:
// the executor drives passes as data + callbacks, not virtual dispatch. Any field may be empty/null
// (a pass that only draws leaves record_compute null; a non-async pass leaves has_async null).
struct PassExec
{
    bool compute_only = false;   // runs standalone in the group loop, after the scene resolve (post)
    bool prepass_only = false;   // runs record_compute in the frame-top prepass, forms no group
    RecordFn record;             // draw / compute-only body (group-loop record)
    ComputeFn record_compute;    // frame-top prepass compute (null = none)
    QueryFn has_async;           // dynamic async gate (null = never async)
    RecordFn record_async;       // async chain body
    UsagesFn usages;             // reads the pass's live logical usages (graph resolves physical)
    UsagesFn async_usages;       // reads the pass's live async logical usages
};

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
    RecordFn record;           // (legacy raster()/compute() body; the executor uses exec.record now)
    PassExec exec;             // the runtime surface the executor drives (retires the Pass* bridge)
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
    PassExec exec;   // the runtime surface (owned by value — callbacks close over the stable passes)
};

struct CompiledFrame
{
    std::vector<CompiledPass> passes;   // in toposorted execution order
    GraphPlan plan;                   // planner output (adjacency / toposort / lifetimes)
};

// Brief 11 M4: introspection — a read-only snapshot of an AUTHORED pass, taken BEFORE the toggle-drop
// so tooling (brief 14's pass panel) lists EVERY pass with its live enabled state + nature, including
// the ones currently disabled. `enabled` is the toggle predicate evaluated live (false = dropped from
// the compiled plan this frame). Pair with GpuProfiler::stats() (keyed by the same name) for timing.
struct PassInfo
{
    std::string_view name;
    PassKind kind;
    bool compute_only;
    bool prepass_only;
    bool enabled;
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
    // Convenience: defaults to SampledRead at the pass's main stage. Use the explicit .read(res,
    // Access, stage) form for anything else (a buffer's StorageRead, vertex/index/indirect fetch,
    // task/mesh-stage reads, storage-image reads). Brief 16 M7: takes a raw resource_id (Handle<*> retired).
    PassSpec& reads(string::gpu::resource_id r) { add_read(r, Access::SampledRead, main_stage(building_.kind), false); return *this; }
    PassSpec& read(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { add_read(r, a, s, false); return *this; }

    // --- requires (essential: a disabled producer transitively skips THIS pass too) --------------
    PassSpec& requires_(string::gpu::resource_id r) { add_read(r, Access::SampledRead, main_stage(building_.kind), true); return *this; }
    PassSpec& require(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { add_read(r, a, s, true); return *this; }

    // --- writes ----------------------------------------------------------------------------------
    PassSpec& writes(string::gpu::resource_id r) { building_.writes.push_back({ r, Access::StorageImageWrite, main_stage(building_.kind) }); return *this; }
    PassSpec& write(string::gpu::resource_id r, Access a, VkPipelineStageFlags2 s) { building_.writes.push_back({ r, a, s }); return *this; }

    // --- attachments (raster) --------------------------------------------------------------------
    PassSpec& color(string::gpu::resource_id r) { building_.writes.push_back({ r, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT }); return *this; }
    PassSpec& depth(string::gpu::resource_id r) { building_.writes.push_back({ r, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT }); return *this; }

    // --- gating ----------------------------------------------------------------------------------
    PassSpec& toggle(EnablePredicate pred) { building_.enabled = std::move(pred); return *this; }
    PassSpec& toggle(const bool* flag) { building_.enabled = [flag] { return *flag; }; return *this; }

    // --- runtime surface + I/O authoring ---------------------------------------------------------
    // Bind the pass's full runtime surface (callbacks + flags + live-usage pointers) — the executor
    // drives passes through this, not a Pass* virtual. `use()` authors an already-declared
    // ResourceUsage for the planner (routing to reads/writes by is_write); the executor reads the
    // LIVE usages via exec.usages, so per-frame-slot buffer variation needs no re-author.
    PassSpec& exec(PassExec e) { building_.exec = std::move(e); return *this; }
    PassSpec& use(const ResourceUsage& u)
    {
        // Brief 16: the planner keys on the usage's stable LOGICAL identity (u.key() — the handle for
        // registry-backed resources, else the raw id). Physical resolution is the executor's job at
        // execute (u.resolve()); the graph orders by logical identity.
        if (is_write(u.access)) building_.writes.push_back({ u.key(), u.access, u.stage });
        else                    building_.reads.push_back({ u.key(), u.access, u.stage, /*required=*/false });
        return *this;
    }

    // --- fluent runtime-surface authoring (the endgame: the app declares each pass's nature here,
    //     retiring the Pass flag-virtuals) ---------------------------------------------------------
    // Nature flags: computeOnly() runs standalone in the group loop after the resolve (post chain);
    // prepass() runs record_compute in the frame-top prepass and forms no group (IBL/GTAO).
    PassSpec& computeOnly() { building_.exec.compute_only = true; return *this; }
    PassSpec& prepass()     { building_.exec.prepass_only = true; return *this; }
    // The draw / compute-only body, and the frame-top prepass-compute body.
    PassSpec& record(RecordFn fn)          { building_.exec.record = std::move(fn); return *this; }
    PassSpec& prepassCompute(ComputeFn fn) { building_.exec.record_compute = std::move(fn); return *this; }
    // The dependency-free async chain: dynamic gate + body + the (live) usages it produces/consumes.
    PassSpec& async(QueryFn has, RecordFn rec, UsagesFn usages)
    {
        building_.exec.has_async = std::move(has);
        building_.exec.record_async = std::move(rec);
        building_.exec.async_usages = std::move(usages);
        return *this;
    }
    // Brief 16: bind the pass's usage GETTER (replaces the retired usagesFrom(&pass->usages) raw
    // pointer). Authors the current usages into the planner NOW by their LOGICAL key() (for the
    // toposort — run at recompile) AND stores the getter so the executor reads them fresh each frame
    // and resolves the per-frame physical via ResourceUsage::resolve(). The graph reads through the
    // getter's interface; it never holds a pointer into a pass's internals.
    PassSpec& usages(UsagesFn get)
    {
        for (const ResourceUsage& u : get()) use(u);
        building_.exec.usages = std::move(get);
        return *this;
    }

    // Terminal: append the built pass to the graph (defined out-of-line — FrameGraph is incomplete here).
    void finish();

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

    // Mint a transient logical id (Brief 16 M7: a raw resource_id — Handle<*> retired). `name` is
    // retained for the future introspection API. image()/buffer() are the same mint now (the graph
    // keys by id, not kind); both are kept so authoring reads intent-fully.
    string::gpu::resource_id image(const std::string& /*name*/)  { return next_transient_++; }
    string::gpu::resource_id buffer(const std::string& /*name*/) { return next_transient_++; }

    // Import an externally-owned resource by its existing id (e.g. COLOR_TARGET, a persistent
    // buffer, a streamed texture) — the graph treats it as an always-available input it never
    // produces.
    string::gpu::resource_id import_image(string::gpu::resource_id id)  { return id; }
    string::gpu::resource_id import_buffer(string::gpu::resource_id id) { return id; }

    PassSpec pass(const std::string& name) { return PassSpec(*this, name); }

    // Enable/disable + graceful-degrade + lowering + toposort. Pure; no GPU. See the .cpp-less
    // definition below (kept inline so this stays header-only for the first increment).
    CompiledFrame compile() const;

    // Brief 11 M4 introspection (read seam for brief 14): enumerate every AUTHORED pass with its
    // metadata + live enabled state (BEFORE the toggle-drop, so disabled passes still appear). Pure;
    // evaluates each toggle predicate now. Order = authoring order.
    std::vector<PassInfo> passes() const
    {
        std::vector<PassInfo> out;
        out.reserve(passes_.size());
        for (const FgPass& p : passes_)
            out.push_back({ p.name, p.kind, p.exec.compute_only, p.exec.prepass_only,
                            !p.enabled || p.enabled() });
        return out;
    }

private:
    friend class PassSpec;
    void add(FgPass&& p) { passes_.push_back(std::move(p)); }
};

inline void PassSpec::raster(RecordFn fn)
{
    building_.kind = PassKind::Raster;
    building_.record = fn;
    building_.exec.record = std::move(fn);
    graph_.add(std::move(building_));
}

inline void PassSpec::compute(RecordFn fn)
{
    building_.kind = PassKind::Compute;
    building_.record = fn;
    building_.exec.record = std::move(fn);
    graph_.add(std::move(building_));
}

inline void PassSpec::finish()
{
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
    PlanBuilder builder;
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
        cp.exec = p.exec;   // by value — callbacks close over the stable pass objects (no dangling)
        frame.passes.push_back(std::move(cp));
    }
    return frame;
}

}  // namespace String
