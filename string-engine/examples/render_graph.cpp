#include <cstdint>
#include <string/vulkan/render_graph.hpp>

#include <algorithm>
#include <unordered_map>
#include <stdexcept>

namespace String
{

PassBuilder::PassBuilder(GraphBuilder& graph_builder, const std::string& name)
: parent_(graph_builder)
, building_(name)
{

}

auto PassBuilder::use(ResourceID resource, Access access, VkPipelineStageFlags2 stage) -> PassBuilder&
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
    return PassBuilder(*this, std::move(name));
}

void GraphBuilder::finish_pass(PassNode&& pass)
{
    passes_.push_back(std::move(pass));
}

void dfs_cycle_detect(uint32_t node, const std::vector<std::vector<uint32_t>>& adj, std::vector<uint32_t>& state, std::vector<uint32_t>& result)
{
    if (state[node] == 2) return;
    if (state[node] == 1) throw std::runtime_error("Cycle detected in render graph");
    state[node] = 1;
    for (int dep : adj[node]) dfs_cycle_detect(dep, adj, state, result);
    state[node] = 2;
    result.push_back(node);
}

auto GraphBuilder::build() -> RenderGraph
{
    RenderGraph graph;
    graph.passes = std::move(passes_);
    const int num_passes = graph.passes.size();
    graph.adjacency.assign(num_passes, {});

    // 1) Collect writers/readers lists per resource (read vs write derived from the Access)
    std::unordered_map<ResourceID, std::vector<uint32_t>> resource_writers;
    std::unordered_map<ResourceID, std::vector<uint32_t>> resource_readers;

    for (int i = 0; i < num_passes; ++i)
    {
        for (const auto& usage : graph.passes[i].usages)
        {
            if (is_write(usage.access))
                resource_writers[usage.resource].push_back(i);
            else
                resource_readers[usage.resource].push_back(i);
        }
    }

    // 2) Build adjacency using writer chain + writer->reader links (single pass)
    for (const auto& [resource_id, writers] : resource_writers)
    {
        // enforce write order if same resource is written multiple times
        for (size_t k = 0; k + 1 < writers.size(); ++k)
            graph.adjacency[writers[k]].push_back(writers[k + 1]);

        // writer -> readers
        auto r_it = resource_readers.find(resource_id);
        if (r_it != resource_readers.end())
        {
            const auto& readers = r_it->second;
            for (int w : writers)
            {
                for (int r : readers)
                {
                    if (w == r) continue;
                    graph.adjacency[w].push_back(r);
                }
            }
        }
    }

    // 3) Deduplicate adjacency lists (sort+unique)
    for (auto& vec : graph.adjacency)
    {
        if (vec.empty()) continue;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }

    // 4) Topological sort with cycle detection (DFS)
    graph.toposorted.clear();
    std::vector<uint32_t> state(num_passes, 0); // 0, 1, 2
    for (uint32_t i = 0; i < num_passes; ++i)
    {
        if (state[i] == 0) dfs_cycle_detect(i, graph.adjacency, state, graph.toposorted);
    }
    std::reverse(graph.toposorted.begin(), graph.toposorted.end());

    // 5) Compute first/last use per resource by iterating passes in topo-order
    graph.resource_lifetimes.clear();
    for (uint32_t pos = 0; pos < graph.toposorted.size(); ++pos)
    {
        uint32_t pass_index = graph.toposorted[pos];
        const PassNode& pass = graph.passes[pass_index];

        for (const auto& usage : pass.usages)
        {
            auto& lifetime = graph.resource_lifetimes[usage.resource];

            lifetime.first = std::min(lifetime.first.value_or(pos), pos);
            lifetime.last = std::max(lifetime.last.value_or(pos), pos);

            if (is_write(usage.access) && !lifetime.first_writer.has_value())
            {
                // earliest writer seen in topo-order
                lifetime.first_writer = pos;
            }
        }
    }

    return graph;
}

}

#include <print>

using namespace String;

int main() {
    // Fake resource IDs for now
    ResourceID depth_id   = 1;
    ResourceID color_id   = 2;
    ResourceID lightgrid_id = 3;

    RenderGraph graph = GraphBuilder()
        .add_pass("depth_prepass")
            .use(depth_id, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT)
            .end_pass()
        .add_pass("light_cull")
            .use(depth_id, Access::DepthRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .use(lightgrid_id, Access::StorageWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .end_pass()
        .add_pass("forward_shading")
            .use(depth_id, Access::DepthRead, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT)
            .use(lightgrid_id, Access::StorageRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
            .use(color_id, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            .end_pass()
        .build();

    for (auto& pass : graph.passes)
    {
        std::println("PassNode: {}", pass.name);
        for (auto& usage : pass.usages)
            std::println("  {} {} ({})", is_write(usage.access) ? "Writes" : "Reads",
                usage.resource, (int) usage.access);
    }

    std::println("Execution order:");
    for (int idx : graph.toposorted)
        std::println("  {}", graph.passes[idx].name);

    return 0;
}
