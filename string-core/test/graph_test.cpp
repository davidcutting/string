#include <gtest/gtest.h>

#include <algorithm>
#include <string/core/graph.hpp>
#include <ranges>
#include <vector>

class DirectedGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        graph.add_edge(0, 1);
        graph.add_edge(1, 2);
        graph.add_edge(0, 2);
        // Self-looping for funzies?
        graph.add_edge(3, 3);
    }

    graph::DirectedGraph graph;
};

TEST_F(DirectedGraphTest, HasEdges)
{
    EXPECT_EQ(graph.node_count(), 4);
    EXPECT_EQ(graph.edge_count(), 4);

    EXPECT_TRUE(graph.has_edge(0, 1));
    EXPECT_TRUE(graph.has_edge(1, 2));
    EXPECT_TRUE(graph.has_edge(0, 2));
    EXPECT_TRUE(graph.has_edge(3, 3));
    EXPECT_FALSE(graph.has_edge(2, 0));
    EXPECT_FALSE(graph.has_edge(1, 0));
}

TEST_F(DirectedGraphTest, Successors)
{
    auto successors_0 = graph.successors(0);
    std::vector<graph::NodeID> succ_0_vec(successors_0.begin(), successors_0.end());

    std::ranges::sort(succ_0_vec);

    EXPECT_EQ(succ_0_vec, std::vector<graph::NodeID>({1, 2}));

    auto successors_1 = graph.successors(1);
    std::vector<graph::NodeID> succ_1_vec(successors_1.begin(), successors_1.end());

    EXPECT_EQ(succ_1_vec, std::vector<graph::NodeID>({2}));
}

TEST_F(DirectedGraphTest, RemoveEdge)
{
    graph.remove_edge(0, 2);
    EXPECT_FALSE(graph.has_edge(0, 2));
    EXPECT_EQ(graph.edge_count(), 3);
}

TEST_F(DirectedGraphTest, View)
{
    auto nodes = graph.nodes();
    std::vector<graph::NodeID> nodes_vec(nodes.begin(), nodes.end());

    std::ranges::sort(nodes_vec);

    EXPECT_EQ(nodes_vec, std::vector<graph::NodeID>({0, 1, 2, 3}));
}

TEST_F(DirectedGraphTest, Compiled)
{
    // Compile the graph
    auto compiled = graph.compile();

    // Test compiled graph properties
    EXPECT_EQ(compiled.node_count(), 4);
    EXPECT_EQ(compiled.edge_count(), 4);

    // Test compiled graph edge existence
    EXPECT_TRUE(compiled.has_edge(0, 1));
    EXPECT_TRUE(compiled.has_edge(1, 2));
    EXPECT_TRUE(compiled.has_edge(0, 2));
    EXPECT_TRUE(compiled.has_edge(3, 3));
    EXPECT_FALSE(compiled.has_edge(2, 0));

    // Test out_degree
    EXPECT_EQ(compiled.out_degree(0), 2);
    EXPECT_EQ(compiled.out_degree(1), 1);
    EXPECT_EQ(compiled.out_degree(2), 0);
    EXPECT_EQ(compiled.out_degree(3), 1);
}

TEST_F(DirectedGraphTest, CompiledSuccessors)
{
    auto compiled = graph.compile();

    // Test compiled successors (should be sorted)
    auto compiled_successors_0 = compiled.successors(0);
    std::vector<graph::NodeID> comp_succ_0_vec(compiled_successors_0.begin(), compiled_successors_0.end());
    EXPECT_EQ(comp_succ_0_vec, std::vector<graph::NodeID>({1, 2}));  // Already sorted
}

TEST_F(DirectedGraphTest, CompiledDFS)
{
    auto compiled = graph.compile();

    // Test DFS traversal view
    auto dfs_traversal = compiled | graph::view::dfs_view(0);
    std::vector<graph::NodeID> dfs_vec(dfs_traversal.begin(), dfs_traversal.end());

    EXPECT_EQ(dfs_vec.size(), 3);  // Should visit 0, 1, 2
    EXPECT_EQ(dfs_vec[0], 0);      // Should start with 0

    EXPECT_TRUE(std::ranges::find(dfs_vec, 1) != dfs_vec.end());
    EXPECT_TRUE(std::ranges::find(dfs_vec, 2) != dfs_vec.end());
}

TEST_F(DirectedGraphTest, CompiledBFS)
{
    auto compiled = graph.compile();

    // Test BFS traversal view
    auto bfs_traversal = compiled | graph::view::bfs_view(0);
    std::vector<graph::NodeID> bfs_vec(bfs_traversal.begin(), bfs_traversal.end());

    EXPECT_EQ(bfs_vec.size(), 3);  // Should visit 0, 1, 2
    EXPECT_EQ(bfs_vec[0], 0);      // Should start with 0

    EXPECT_TRUE(std::ranges::find(bfs_vec, 1) != bfs_vec.end());
    EXPECT_TRUE(std::ranges::find(bfs_vec, 2) != bfs_vec.end());
}

TEST_F(DirectedGraphTest, CompiledTopoSort)
{
    auto compiled = graph.compile();

    // Test topological sort
    auto topo_sort = compiled | graph::view::topological_view;
    EXPECT_TRUE(topo_sort.is_valid());  // Should be valid (no cycles except self-loop)

    std::vector<graph::NodeID> topo_vec(topo_sort.begin(), topo_sort.end());
    // In a valid topological order, 0 should come before 1, and 1 should come
    // before 2
    auto pos_0 = std::ranges::find(topo_vec, 0) - topo_vec.begin();
    auto pos_1 = std::ranges::find(topo_vec, 1) - topo_vec.begin();
    auto pos_2 = std::ranges::find(topo_vec, 2) - topo_vec.begin();

    EXPECT_LT(pos_0, pos_1);  // 0 should come before 1
    EXPECT_LT(pos_1, pos_2);  // 1 should come before 2
}

TEST_F(DirectedGraphTest, CompiledEdges)
{
    auto compiled = graph.compile();

    // Test edges view
    std::vector<graph::Edge> edges_vec;
    for (auto edge : compiled.edges()) {
        edges_vec.push_back(edge);
    }
    EXPECT_EQ(edges_vec.size(), 4);

    // Verify specific edges exist
    EXPECT_TRUE(std::ranges::any_of(edges_vec, [](const graph::Edge &e) { return e.from == 0 && e.to == 1; }));
    EXPECT_TRUE(std::ranges::any_of(edges_vec, [](const graph::Edge &e) { return e.from == 1 && e.to == 2; }));
    EXPECT_TRUE(std::ranges::any_of(edges_vec, [](const graph::Edge &e) { return e.from == 0 && e.to == 2; }));
    EXPECT_TRUE(std::ranges::any_of(edges_vec, [](const graph::Edge &e) { return e.from == 3 && e.to == 3; }));
}

TEST_F(DirectedGraphTest, SerializeGraphViz)
{
    auto compiled = graph.compile();

    std::string dot_string_actual = graph::to_dot(compiled);

    std::string expected =
        "digraph G {\n"
        "    0;\n"
        "    1;\n"
        "    2;\n"
        "    3;\n"
        "    0 -> 1;\n"
        "    0 -> 2;\n"
        "    1 -> 2;\n"
        "    3 -> 3;\n"
        "}\n";

    EXPECT_EQ(dot_string_actual, expected);    
}