#include <cstdint>
#include <cstdlib>
#include <string/voronoi.hpp>


int main()
{
    auto map = voronoi::Map<std::uint32_t>({10, 11});

    voronoi::print_map(map);

    return EXIT_SUCCESS;
}