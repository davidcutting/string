#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <string/vulkan/resource.hpp>

namespace String
{

class GraphBuilder;

enum class Access : std::uint8_t
{
    DepthStencilRead,
    DepthStencilWrite,
    ColorRead,
    ColorWrite,
    StorageRead,
    StorageWrite,
};

struct Dependency
{
    ResourceID resource;
    Access access;
};

struct ResourceLifetime
{
    std::optional<uint32_t> first_writer;
    std::optional<uint32_t> first;
    std::optional<uint32_t> last;
};

struct Pass
{
    std::string name;
    std::vector<Dependency> reads;
    std::vector<Dependency> writes;
};

struct RenderGraph
{
    std::vector<Pass> passes;
    std::vector<std::vector<uint32_t>> adjacency;
    std::vector<uint32_t> toposorted;
    std::unordered_map<ResourceID, ResourceLifetime> resource_lifetimes;
};

class PassBuilder
{
    GraphBuilder& parent_;
    Pass building_;
public:
    explicit PassBuilder(GraphBuilder& graph_builder, const std::string& name);

    auto reads(const ResourceID& resource, const Access& access) -> PassBuilder&;
    auto writes(const ResourceID& resource, const Access& access) -> PassBuilder&;

    auto end_pass() -> GraphBuilder&;
};

class GraphBuilder
{
    std::vector<Pass> passes_;
public:
    auto add_pass(const std::string& name) -> PassBuilder;

    auto build() -> RenderGraph;
private:
    friend PassBuilder;
    void finish_pass(Pass&& pass);
};

}