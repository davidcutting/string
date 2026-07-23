#include <string/vulkan/render_graph.hpp>

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace String
{

// Brief 04e M2: the planner implementation moved from the (unbuilt) example into the library —
// the renderer now builds a RenderGraph from the frame passes' declared usages every frame and
// executes in its toposorted order, so declared dependencies are the single source of truth for
// scheduling (and, via the tracker, barriers).

PassBuilder::PassBuilder(GraphBuilder& graph_builder, const std::string& name)
: parent_(graph_builder)
, building_(name)
{
}

auto PassBuilder::use(string::gpu::resource_id resource, Access access, VkPipelineStageFlags2 stage)
    -> PassBuilder&
{
    building_.usages.push_back({ resource, access, stage });
    return *this;
}

auto PassBuilder::end_pass() -> GraphBuilder&
{
    parent_.finish_pass(std::move(building_));
    return parent_;
}

auto GraphBuilder::add_pass(const std::string& name) -> PassBuilder
{
    return PassBuilder(*this, name);
}

void GraphBuilder::finish_pass(PassNode&& pass)
{
    passes_.push_back(std::move(pass));
}

auto GraphBuilder::build() -> RenderGraph
{
    RenderGraph graph;
    graph.passes = std::move(passes_);
    const uint32_t num_passes = static_cast<uint32_t>(graph.passes.size());
    graph.adjacency.assign(num_passes, {});

    // 1) Collect writers/readers lists per resource (read vs write derived from the Access).
    std::unordered_map<string::gpu::resource_id, std::vector<uint32_t>> resource_writers;
    std::unordered_map<string::gpu::resource_id, std::vector<uint32_t>> resource_readers;
    for (uint32_t i = 0; i < num_passes; ++i)
    {
        for (const ResourceUsage& usage : graph.passes[i].usages)
        {
            // A pass may declare several usages of one resource (e.g. a write in its compute
            // stage AND a write in its draw stages) — it is still ONE writer/reader; a
            // duplicate entry would forge a self-edge (a false "cycle").
            auto& list = is_write(usage.access) ? resource_writers[usage.resource]
                                               : resource_readers[usage.resource];
            if (list.empty() || list.back() != i) list.push_back(i);
        }
    }

    // 2) Adjacency: writer-chain order (WAW) + writer->reader (RAW) + reader->NEXT-writer (WAR:
    //    a reader declared before a later writer must complete before that writer clobbers).
    for (const auto& [resource_id, writers] : resource_writers)
    {
        for (size_t k = 0; k + 1 < writers.size(); ++k)
            graph.adjacency[writers[k]].push_back(writers[k + 1]);

        auto r_it = resource_readers.find(resource_id);
        if (r_it == resource_readers.end()) continue;
        for (uint32_t w : writers)
        {
            for (uint32_t r : r_it->second)
            {
                if (w == r) continue;
                // Declaration order carries intent for same-resource read/write pairs: a writer
                // precedes readers declared after it; a reader precedes writers declared after it.
                if (w < r) graph.adjacency[w].push_back(r);
                else graph.adjacency[r].push_back(w);
            }
        }
    }

    // 3) Deduplicate adjacency lists.
    for (auto& vec : graph.adjacency)
    {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }

    // 4) STABLE topological sort (Kahn's algorithm, smallest declaration index first). When the
    //    authored order is itself a valid topological order — the common case — the execution
    //    order EQUALS it, which is what the byte-parity gates rely on. Cycles throw.
    std::vector<uint32_t> indegree(num_passes, 0);
    for (const auto& vec : graph.adjacency)
        for (uint32_t to : vec) ++indegree[to];
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
    for (uint32_t i = 0; i < num_passes; ++i)
        if (indegree[i] == 0) ready.push(i);
    graph.toposorted.clear();
    graph.toposorted.reserve(num_passes);
    while (!ready.empty())
    {
        const uint32_t node = ready.top();
        ready.pop();
        graph.toposorted.push_back(node);
        for (uint32_t to : graph.adjacency[node])
            if (--indegree[to] == 0) ready.push(to);
    }
    if (graph.toposorted.size() != num_passes)
        throw std::runtime_error("Cycle detected in render graph");

    // 5) Resource lifetimes in topo positions (first/last touch + first writer) — the M3
    //    transient-aliasing input.
    graph.resource_lifetimes.clear();
    for (uint32_t pos = 0; pos < graph.toposorted.size(); ++pos)
    {
        const PassNode& pass = graph.passes[graph.toposorted[pos]];
        for (const ResourceUsage& usage : pass.usages)
        {
            ResourceLifetime& lifetime = graph.resource_lifetimes[usage.resource];
            lifetime.first = std::min(lifetime.first.value_or(pos), pos);
            lifetime.last = std::max(lifetime.last.value_or(pos), pos);
            if (is_write(usage.access) && !lifetime.first_writer.has_value())
                lifetime.first_writer = pos;
        }
    }

    return graph;
}

}  // namespace String
