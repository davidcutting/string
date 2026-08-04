#include <string/vulkan/graph_introspect.hpp>

namespace String
{
namespace
{
const GraphIntrospect* g_global = nullptr;
}

void GraphIntrospect::set_global(const GraphIntrospect* p) { g_global = p; }
const GraphIntrospect* GraphIntrospect::global() { return g_global; }

}  // namespace String
