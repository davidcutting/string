#pragma once

#include <cstdint>
#include <print>

namespace voronoi
{

struct Extent2D
{
    uint32_t x;
    uint32_t y;
};

// this is square
template<typename T>
class Map
{
    T* data_;
    Extent2D extent_;
public:
    explicit Map(const Extent2D& extent)
    : extent_(extent)
    {
        data_ = new T(extent.x * extent.y);
    }

    ~Map()
    {
        delete data_;
    }

    T at(const uint32_t& x, const uint32_t& y) const
    {
        const auto index = x * extent_.x + y;
        return data_[index];
    }

    Extent2D get_extent() const
    {
        return extent_;
    }
};

enum class VoronoiState : std::uint8_t
{
    FREE,
    NOT_FREE,
};

struct Cell
{
    uint32_t x;
    uint32_t y;
    uint32_t distance;
    bool dirty; // if the cell needs updating
    bool is_edge;
};

struct BoundingBox
{
    uint8_t length;
    uint8_t width;
    Cell origin;
};

template<typename T>
void print_map(const Map<T>& map)
{
    const auto extent = map.get_extent();

    for (int i = 0; i < extent.x; i++)
    {
        for (int j = 0; j < extent.y; j++)
        {
            std::print("{} ", map.at(i, j));
        }
        std::println();
    }
}

//void neighbors_4(const Map& map, const BoundingBox& bb);

}