#pragma once

#include <cstdint>
#include <stack>
#include <string_view>
#include <vector>

#include <string/math/math.hpp>

namespace string
{

struct dimension
{
    uint16_t width;
    uint16_t height;
};

struct bounding_box
{
    uint16_t x;
    uint16_t y;
    dimension dimension;
};

struct padding
{
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
};

struct color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct id
{
    std::string_view name;
};

enum class alignment : uint8_t
{
    TOP,
    LEFT,
    CENTER,
    RIGHT,
    BOTTOM,
};

enum class direction : uint8_t
{
    VERTICAL,
    HORIZONTAL,
};

enum class shape : uint8_t
{
    CIRCLE,
    RECTANGLE,
    ROUNDED_RECTANGLE,
};

struct element
{
    id id;
    color color;
    uint16_t radius;
    shape shape;
};

struct format
{
    padding padding;
    uint16_t gap;
    alignment alignment;
    direction direction;
};

struct layout
{
    vec2 cursor;
    vec2 offset;
    format format;
};

struct renderable_element
{
    element element;
    bounding_box bounding_box;
};

void compute_along_axis_sizing();
void compute_cross_axis_sizing();

class layout_builder
{
    std::stack<layout> layouts;
    std::vector<element> elements;
public:
    auto begin(const format& format) -> layout_builder&;
    auto add_element(const element& element) -> layout_builder&;
    auto end() -> layout_builder&;
};

}