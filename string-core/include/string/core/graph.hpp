#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>

namespace graph {

using NodeID = std::uint16_t;

struct Edge {
    NodeID from;
    NodeID to;
};

class CompiledGraph;

template <typename G>
concept DirectedGraphLike = requires(const G &g, NodeID node) {
    { g.successors(node) } -> std::ranges::range;
    { g.nodes() } -> std::ranges::range;
    { g.has_edge(node, node) } -> std::convertible_to<bool>;
};

class DirectedGraph {
private:
    std::unordered_map<NodeID, std::unordered_set<NodeID>> adjacency_list_;

public:
    void add_edge(NodeID from, NodeID to) {
        adjacency_list_[from].insert(to);
        // Ensure 'to' node exists even if it has no outgoing edges
        adjacency_list_[to];
    }

    void remove_edge(NodeID from, NodeID to) {
        if (auto it = adjacency_list_.find(from); it != adjacency_list_.end()) {
            it->second.erase(to);
        }
    }

    bool has_edge(NodeID from, NodeID to) const {
        if (auto it = adjacency_list_.find(from); it != adjacency_list_.end()) {
            return it->second.contains(to);
        }
        return false;
    }

    auto successors(NodeID node) const {
        // Can't just look up adjacency_list[node] because it has consequences.
        // This function is const
        if (auto it = adjacency_list_.find(node); it != adjacency_list_.end()) {
            return std::views::all(it->second);
        }
        static const std::unordered_set<NodeID> empty;
        return std::views::all(empty);
    }

    auto nodes() const { return adjacency_list_ | std::views::keys; }

    size_t node_count() const { return adjacency_list_.size(); }

    size_t edge_count() const {
        size_t total = 0;
        for (const auto &[_, successors] : adjacency_list_) {
            total += successors.size();
        }
        return total;
    }

    auto compile() const -> CompiledGraph;

private:
    friend class CompiledGraph;
};

class CompiledGraph {
private:
    std::vector<size_t> offsets_;
    std::vector<NodeID> destinations_;
    NodeID max_node_id_;

public:
    CompiledGraph(const DirectedGraph &directed_graph) : max_node_id_(0) {
        const auto &adj_list = directed_graph.adjacency_list_;

        max_node_id_ = std::ranges::max(adj_list | std::views::keys);

        // Calculate total edges for reservation
        size_t total_edges = 0;
        for (const auto& successors : adj_list | std::views::values) {
            total_edges += successors.size();
        }

        offsets_.resize(static_cast<size_t>(max_node_id_) + 2, 0);
        destinations_.reserve(total_edges);

        size_t offset = 0;
        for (NodeID node = 0; node <= max_node_id_; ++node) {
            offsets_[node] = offset;

            if (auto it = adj_list.find(node); it != adj_list.end()) {
                const auto &successors = it->second;

                // Sort successors for better cache locality and deterministic ordering
                std::vector<NodeID> sorted_successors(successors.begin(), successors.end());
                std::ranges::sort(sorted_successors);

                destinations_.insert(destinations_.end(), sorted_successors.begin(), sorted_successors.end());
                offset += sorted_successors.size();
            }
        }
        offsets_[max_node_id_ + 1] = offset;
    }

    auto successors(NodeID node) const {
        if (node <= max_node_id_) {
            size_t start = offsets_[node];
            size_t end = offsets_[node + 1];
            return std::span(destinations_.data() + start, end - start);
        }
        return std::span<const NodeID>{};
    }

    bool has_edge(NodeID from, NodeID to) const {
        auto succ = successors(from);
        return std::ranges::binary_search(succ, to);
    }

    size_t out_degree(NodeID node) const { return successors(node).size(); }

    auto nodes() const {
        return std::views::iota(NodeID{0}, static_cast<NodeID>(max_node_id_ + 1)) |
               std::views::filter([this](NodeID node) {
                   return offsets_[node] != offsets_[node + 1] ||
                          std::ranges::any_of(destinations_, [node](NodeID dest) { return dest == node; });
               });
    }

    auto edges() const {
        return std::views::iota(NodeID{0}, static_cast<NodeID>(max_node_id_ + 1)) |
               std::views::transform([this](NodeID from) {
                   const size_t start = offsets_[from];
                   const size_t end = offsets_[from + 1];

                   return std::views::iota(start, end) | std::views::transform([this, from](size_t dest_idx) {
                              return Edge{from, destinations_[dest_idx]};
                          });
               }) |
               std::views::join;
    }

    size_t node_count() const { return std::ranges::distance(nodes()); }

    size_t edge_count() const { return destinations_.size(); }

private:
    friend class DirectedGraph;
};

inline auto DirectedGraph::compile() const -> CompiledGraph { return CompiledGraph(*this); }

template <DirectedGraphLike G>
std::string to_dot(const G& g, std::string_view graph_name = "G")
{
    std::ostringstream out;
    out << "digraph " << graph_name << " {\n";

    for (NodeID node : g.nodes()) {
        out << "    " << node << ";\n";
    }

    for (NodeID from : g.nodes()) {
        for (NodeID to : g.successors(from)) {
            out << "    " << from << " -> " << to << ";\n";
        }
    }

    out << "}\n";
    return out.str();
}

}  // namespace graph

#include <queue>
#include <stack>

namespace graph::view {

template <DirectedGraphLike G>
class dfs_view_t : public std::ranges::view_interface<dfs_view_t<G>> {
private:
    const G *graph_;
    NodeID start_;

    struct iterator {
        using iterator_category = std::input_iterator_tag;
        using value_type = NodeID;
        using difference_type = std::ptrdiff_t;
        using pointer = const NodeID*;
        using reference = const NodeID&;

        const G *graph_;
        std::stack<NodeID> stack_;
        std::unordered_set<NodeID> visited_;
        NodeID current_;
        bool at_end_;

        iterator(const G *g, NodeID start) : graph_(g), at_end_(false) {
            if (g) {
                stack_.push(start);
                advance_to_next();  // Properly initialize first element
            } else {
                at_end_ = true;
            }
        }

        iterator() : graph_(nullptr), at_end_(true) {}

        void advance_to_next() {
            while (!stack_.empty()) {
                auto node = stack_.top();
                stack_.pop();

                if (!visited_.contains(node)) {
                    visited_.insert(node);
                    current_ = node;

                    // Add successors to stack for future visits
                    for (auto successor : graph_->successors(node)) {
                        if (!visited_.contains(successor)) {
                            stack_.push(successor);
                        }
                    }
                    return;
                }
            }
            at_end_ = true;
        }

        auto operator*() const { return current_; }

        iterator &operator++() {
            advance_to_next();
            return *this;
        }

        bool operator==(const iterator &other) const { return at_end_ && other.at_end_; }
    };

public:
    dfs_view_t(const G &graph, NodeID start) : graph_(&graph), start_(start) {}

    auto begin() const { return iterator(graph_, start_); }
    auto end() const { return iterator(); }
};

template <DirectedGraphLike G>
class bfs_view_t : public std::ranges::view_interface<bfs_view_t<G>> {
private:
    const G *graph_;
    NodeID start_;

    struct iterator {
        using iterator_category = std::input_iterator_tag;
        using value_type = NodeID;
        using difference_type = std::ptrdiff_t;
        using pointer = const NodeID*;
        using reference = const NodeID&;

        const G *graph_;
        std::queue<NodeID> queue_;
        std::unordered_set<NodeID> visited_;
        NodeID current_;

        iterator(const G *g, NodeID start) : graph_(g), current_(start) {
            if (g) {
                queue_.push(start);
                visited_.insert(start);
            }
        }

        iterator() : graph_(nullptr) {}

        auto operator*() const { return current_; }

        iterator &operator++() {
            if (!queue_.empty()) {
                auto node = queue_.front();
                queue_.pop();

                for (auto successor : graph_->successors(node)) {
                    if (!visited_.contains(successor)) {
                        visited_.insert(successor);
                        queue_.push(successor);
                    }
                }

                if (!queue_.empty()) {
                    current_ = queue_.front();
                } else {
                    graph_ = nullptr;  // Marks the end
                }
            } else {
                graph_ = nullptr;
            }

            return *this;
        }

        bool operator==(const iterator &other) const {
            return (graph_ == nullptr && other.graph_ == nullptr);
        }
    };

public:
    bfs_view_t(const G &graph, NodeID start) : graph_(&graph), start_(start) {}

    auto begin() const { return iterator(graph_, start_); }
    auto end() const { return iterator(); }
};

template <DirectedGraphLike G>
class topological_view_t : public std::ranges::view_interface<topological_view_t<G>> {
private:
    std::vector<NodeID> sorted_nodes_;

public:
    topological_view_t(const G &graph) {
        // Kahn's algorithm
        std::unordered_map<NodeID, int> in_degree;

        // Initialize in-degrees
        for (auto node : graph.nodes()) {
            in_degree[node] = 0;
        }

        for (auto node : graph.nodes()) {
            for (auto successor : graph.successors(node)) {
                in_degree[successor]++;
            }
        }

        // Find nodes with no incoming edges
        std::queue<NodeID> queue;
        for (auto [node, degree] : in_degree) {
            if (degree == 0) {
                queue.push(node);
            }
        }

        // Process nodes
        while (!queue.empty()) {
            auto node = queue.front();
            queue.pop();
            sorted_nodes_.push_back(node);

            for (auto successor : graph.successors(node)) {
                in_degree[successor]--;
                if (in_degree[successor] == 0) {
                    queue.push(successor);
                }
            }
        }
    }

    auto begin() const { return sorted_nodes_.begin(); }
    auto end() const { return sorted_nodes_.end(); }
    bool empty() const { return sorted_nodes_.empty(); }
    bool is_valid() const { return !sorted_nodes_.empty(); }  // Has no cycles
};

// -------------------------------------------------------------

struct dfs_view_closure {
    NodeID start;
    constexpr explicit dfs_view_closure(NodeID s) : start(s) {}

    template <DirectedGraphLike G>
    constexpr auto operator()(const G& graph) const {
        return dfs_view_t<G>{graph, start};
    }
};

struct dfs_view_fn {
    constexpr auto operator()(NodeID s) const {
        return dfs_view_closure{s};
    }
};

inline constexpr dfs_view_fn dfs_view;

// -------------------------------------------------------------

struct bfs_view_closure {
    NodeID start;
    constexpr explicit bfs_view_closure(NodeID s) : start(s) {}

    template <DirectedGraphLike G>
    constexpr auto operator()(const G& graph) const {
        return bfs_view_t<G>{graph, start};
    }
};

struct bfs_view_fn {
    constexpr auto operator()(NodeID s) const {
        return bfs_view_closure{s};
    }
};

inline constexpr bfs_view_fn bfs_view;

// -------------------------------------------------------------

struct topological_view_fn {
    template <DirectedGraphLike G>
    constexpr auto operator()(const G& graph) const {
        return topological_view_t<G>(graph);
    }
};

inline constexpr topological_view_fn topological_view;

}  // namespace graph::view

template <graph::DirectedGraphLike G>
constexpr auto operator|(const G& graph, const graph::view::bfs_view_closure& closure) {
    return closure(graph);
}

template <graph::DirectedGraphLike G>
constexpr auto operator|(const G& graph, const graph::view::dfs_view_closure& closure) {
    return closure(graph);
}

template <graph::DirectedGraphLike G>
constexpr auto operator|(const G& graph, const graph::view::topological_view_fn& fn) {
    return fn(graph);
}