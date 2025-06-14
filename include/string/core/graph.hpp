#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <queue>
#include <stack>
#include <limits>
#include <algorithm>
#include <ranges>

using NodeID = std::uint16_t;
constexpr NodeID MAXIMUM_INDEX = std::numeric_limits<NodeID>::max();

template<typename T>
struct Node
{
    T data;
    bool is_active;
};

struct Edge {
    NodeID parent;
    NodeID child;
};

inline bool operator==(const Edge& one, const Edge& two)
{
    return one.parent == two.parent && one.child == two.child;
}

struct MemoryStats {
    size_t total_nodes = 0;
    size_t active_nodes = 0;
    size_t free_slots = 0;
    size_t memory_bytes = 0;
    double fragmentation_ratio = 0.0;
};

template<typename T>
class MultiTree
{
    std::vector<Node<T>> nodes_;
    std::vector<Edge> edges_;
    std::vector<NodeID> free_indices_;
    size_t active_count_ = 0;

public:
    size_t size() const { return active_count_; }
    size_t capacity() const { return nodes_.size(); }
    bool empty() const { return active_count_ == 0; }
    size_t edge_count() const { return edges_.size(); }
    
    // Node management
    NodeID create_node()
    {
        NodeID index;

        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
            nodes_[index] = Node<T>{ .is_active = true };
        } else {
            index = nodes_.size();
            nodes_.emplace_back();
        }
        
        ++active_count_;
        return index;
    }
    
    NodeID create_node(const T& data)
    {
        NodeID index = create_node();
        nodes_[index].data = data;
        return index;
    }
    
    NodeID create_node(T&& data) {
        NodeID index = create_node();
        nodes_[index].data = std::move(data);
        return index;
    }
    
    void remove_node(const NodeID& index) {
        if (!is_valid_index(index)) return;
        
        // Remove all edges involving this node
        std::erase_if(edges_, [index](const Edge& e) {
            return e.parent == index || e.child == index;
        });
        
        // Mark as inactive and add to free list
        nodes_[index].is_active = false;
        free_indices_.push_back(index);
        --active_count_;
    }

    bool add_edge(const NodeID& parent, const NodeID& child) {
        if (!is_valid_index(parent) || !is_valid_index(child) || parent == child) {
            return false;
        }
        
        // Check if edge already exists
        Edge new_edge{parent, child};
        if (std::ranges::find(edges_, new_edge) == edges_.end()) {
            return false;
        }

        // Check for cycles, to remain acyclic!
        if (is_cyclic(parent, child)) {
            return false;
        }
        
        edges_.push_back(new_edge);
        return true;
    }
    
    bool remove_edge(const NodeID& parent, const NodeID& child) {
        if (!is_valid_index(parent) || !is_valid_index(child)) {
            return false;
        }
        
        Edge target{parent, child};
        auto it = std::ranges::find(edges_, target);
        if (it != edges_.end()) {
            edges_.erase(it);
            return true;
        }
        return false;
    }

    bool is_valid_index(const NodeID& index) const {
        return index < nodes_.size() && nodes_[index].is_active;
    }
    
    const Node<T>& get_node(const NodeID& index) const {
        assert(is_valid_index(index));
        return nodes_[index];
    }
    
    Node<T>& get_node(NodeID index) {
        assert(is_valid_index(index));
        return nodes_[index];
    }

    std::vector<NodeID> get_children(const NodeID& index) const {
        if (!is_valid_index(index)) {
            return {};
        }
        
        std::vector<NodeID> children;
        for (const auto& edge : edges_) {
            if (edge.parent == index) {
                children.push_back(edge.child);
            }
        }
        return children;
    }

    std::vector<NodeID> get_parents(const NodeID& index) const {
        if (!is_valid_index(index)) {
            return {};
        }
        
        std::vector<NodeID> parents;
        for (const auto& edge : edges_) {
            if (edge.child == index) {
                parents.push_back(edge.parent);
            }
        }
        return parents;
    }

    bool are_siblings(const NodeID& child_a, const NodeID& child_b) const {
        if (!is_valid_index(child_a) || !is_valid_index(child_b) || child_a == child_b) {
            return false;
        }
        
        auto parents_a = get_parents(child_a);
        auto parents_b = get_parents(child_b);

        for (NodeID parent_a : parents_a) {
            if (std::ranges::find(parents_b, parent_a) != parents_b.end()) {
                return true;
            }
        }
        return false;
    }

    size_t get_depth(NodeID index) const {
        if (!is_valid_index(index)) return 0;
        
        size_t max_depth = 0;
        auto parents = get_parents(index);
        
        for (NodeID parent : parents) {
            max_depth = std::max(max_depth, get_depth(parent) + 1);
        }
        
        return max_depth;
    }
    
    size_t max_depth() const {
        size_t depth = 0;
        for (NodeID node : nodes()) {
            depth = std::max(depth, get_depth(node));
        }
        return depth;
    }

    bool is_root(NodeID index) const {
        return is_valid_index(index) && get_parents(index).empty();
    }
    
    bool is_leaf(NodeID index) const {
        return is_valid_index(index) && get_children(index).empty();
    }

    const std::vector<Edge>& edges() const { return edges_; }
    
    auto nodes() const {
        return std::views::iota(NodeID{0}, nodes_.size()) 
             | std::views::filter([this](NodeID idx) { return is_valid_index(idx); });
    }
    
    auto roots() const {
        return nodes() | std::views::filter([this](NodeID idx) {
            return std::ranges::none_of(edges_, [idx](const Edge& e) { 
                return e.child == idx; 
            });
        });
    }
    
    auto leaves() const {
        return nodes() | std::views::filter([this](NodeID idx) {
            return std::ranges::none_of(edges_, [idx](const Edge& e) { 
                return e.parent == idx; 
            });
        });
    }

    const MemoryStats get_memory_stats() const {
        const size_t total = nodes_.size();
        const size_t free = free_indices_.size();
        const double fragmentation = total > 0 ? static_cast<double>(free) / total : 0.0;
        
        return {
            .total_nodes = total,
            .active_nodes = active_count_,
            .free_slots = free,
            .memory_bytes = nodes_.capacity() * sizeof(Node<T>) +
                            edges_.capacity() * sizeof(Edge) +
                            free_indices_.capacity() * sizeof(NodeID),
            .fragmentation_ratio = fragmentation
        };
    }

private:
    bool is_cyclic(NodeID parent, NodeID child) const {
        if (!is_valid_index(parent) || !is_valid_index(child)) {
            return false;
        }
        
        // If child is an ancestor of parent, adding edge would create cycle
        return is_ancestor(child, parent);
    }

    bool is_ancestor(NodeID ancestor, NodeID descendant) const {
        if (!is_valid_index(ancestor) || !is_valid_index(descendant)) {
            return false;
        }
        
        if (ancestor == descendant) {
            return true;
        }
        
        std::queue<NodeID> to_visit;
        std::unordered_set<NodeID> visited;
        
        to_visit.push(ancestor);
        visited.insert(ancestor);
        
        while (!to_visit.empty()) {
            NodeID current = to_visit.front();
            to_visit.pop();
            
            for (NodeID child : get_children(current)) {
                if (child == descendant) {
                    return true;
                }
                if (visited.find(child) == visited.end()) {
                    visited.insert(child);
                    to_visit.push(child);
                }
            }
        }
        
        return false;
    }
};

namespace algo
{

template<typename TreeType, typename VisitFunc>
void dfs(const TreeType& tree, NodeID start_node, VisitFunc visit) {
    if (!tree.is_valid_index(start_node)) return;
    
    std::unordered_set<NodeID> visited;
    std::stack<NodeID> stack;
    stack.push(start_node);
    
    while (!stack.empty()) {
        NodeID current = stack.top();
        stack.pop();
        
        if (visited.find(current) == visited.end()) {
            visited.insert(current);
            visit(tree.get_node(current).data);
            
            // Add children to stack (in reverse order for left-to-right traversal)
            auto children = tree.get_children(current);
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (tree.is_valid_index(*it) && visited.find(*it) == visited.end()) {
                    stack.push(*it);
                }
            }
        }
    }
}

template<typename TreeType, typename VisitFunc>
void bfs(const TreeType& tree, NodeID start_node, VisitFunc visit) {
    if (!tree.is_valid_index(start_node)) return;
    
    std::unordered_set<NodeID> visited;
    std::queue<NodeID> queue;
    queue.push(start_node);
    visited.insert(start_node);
    
    while (!queue.empty()) {
        NodeID current = queue.front();
        queue.pop();
        
        visit(tree.get_node(current).data);
        
        auto children = tree.get_children(current);
        for (NodeID child : children) {
            if (tree.is_valid_index(child) && visited.find(child) == visited.end()) {
                visited.insert(child);
                queue.push(child);
            }
        }
    }
}

}
