#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Brief 14 M2/M3 — a read-only snapshot of the COMPILED frame graph, for the debug UI.
//
// WHY A PROCESS-GLOBAL HANDLE: this mirrors `GpuProfiler::set_global` exactly, and for the same
// stated reason — so a UI/tooling layer can read engine numbers "without the renderer being plumbed
// through the UI author". The debug panels already consume the profiler that way. A global is a
// blunt instrument; inventing a second, different channel next to an existing one that works is
// worse. Read-only, debug-only, one direction (engine -> UI).
//
// NOT a live view into the planner: a plain value refilled when the graph recompiles (toggle flip /
// resize), which is the only time any of it changes.
namespace string
{

class GraphIntrospect
{
public:
    // One resource the compiled plan touches, with its lifetime as TOPO POSITIONS into `passes`.
    struct Resource
    {
        // Derived, not stored anywhere in the engine — see the note in refresh_introspection().
        std::string label;
        bool is_image = false;
        std::uint32_t first = 0;         // first pass position that touches it
        std::uint32_t last = 0;          // last
        std::optional<std::uint32_t> first_writer;   // nullopt = external input (nobody writes it)
    };

    // What one pass does to one resource. `resource` indexes `resources`, and is filled AFTER that
    // vector is sorted, so the index stays valid.
    struct Use
    {
        std::uint32_t resource = 0;
        bool write = false;
    };

    struct Pass
    {
        std::string name;
        std::vector<Use> uses;
        // Compiled positions this pass feeds, straight from `plan.adjacency` remapped out of
        // PLANNER index space into this (toposorted) one. It is the same array the barrier
        // derivation runs on, which is what makes the DAG ground truth rather than a redrawing of
        // someone's mental model.
        std::vector<std::uint32_t> successors;
        // Longest path from any source. The DAG view draws one row per depth — the only honest
        // layout available without absolute placement, and a meaningful one: depth is the earliest
        // point this pass could run, so passes sharing a row are genuinely independent.
        std::uint32_t depth = 0;
    };

    // Compiled passes in EXECUTION (toposorted) order. This — not the authored pass list — is the
    // axis the lifetime positions index; the authored list includes toggled-off passes, so using it
    // would point every bar at the wrong pass.
    std::vector<Pass> passes;
    // Sorted by (first, last) so the lifetime chart reads top-left to bottom-right.
    std::vector<Resource> resources;
    // Authored passes that did NOT reach the compiled plan — toggled off, or transitively skipped
    // because a `.requires()` producer was. The DAG can only draw what compiled, so naming what
    // fell out is how "what else dies if I toggle this off" gets answered.
    std::vector<std::string> dropped;

    // Registered by the renderer on construction, cleared on destruction. Null before the renderer
    // exists / after it is gone — callers must check.
    static void set_global(const GraphIntrospect* p);
    static const GraphIntrospect* global();
};

}  // namespace string
