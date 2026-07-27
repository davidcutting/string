#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <string/gpu/resource.hpp>
#include <string/vulkan/resource_usage.hpp>

namespace String
{

class PlanBuilder;

struct ResourceLifetime
{
    std::optional<uint32_t> first_writer;
    std::optional<uint32_t> first;
    std::optional<uint32_t> last;
};

// A planner node: a named pass plus its typed resource usages — the *same* ResourceUsage the
// executable Pass declares (one model, shared). Distinct from the executable Pass, which also
// records commands; connecting the two is a later stage.
struct PassNode
{
    std::string name;
    std::vector<ResourceUsage> usages;
};

struct GraphPlan
{
    std::vector<PassNode> passes;
    std::vector<std::vector<uint32_t>> adjacency;
    std::vector<uint32_t> toposorted;
    std::unordered_map<string::gpu::resource_id, ResourceLifetime> resource_lifetimes;
};

class PassBuilder
{
    PlanBuilder& parent_;
    PassNode building_;
public:
    explicit PassBuilder(PlanBuilder& graph_builder, const std::string& name);

    // Declare a resource use. Read vs write is derived from the Access (is_write).
    auto use(string::gpu::resource_id resource, Access access, VkPipelineStageFlags2 stage) -> PassBuilder&;

    auto end_pass() -> PlanBuilder&;
};

class PlanBuilder
{
    std::vector<PassNode> passes_;
public:
    auto add_pass(const std::string& name) -> PassBuilder;

    auto build() -> GraphPlan;
private:
    friend PassBuilder;
    void finish_pass(PassNode&& pass);
};

}
