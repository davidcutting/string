#include <string/core/layout.hpp>

namespace string
{

void compute_along_axis_sizing()
{
    // sum of children
}

void compute_cross_axis_sizing()
{
    // maximum of children
}

auto layout_builder::begin(const format& format) -> layout_builder&
{
    layout new_layout;
    new_layout.format = format;
    layouts.push(std::move(new_layout));

    return *this;
}

auto layout_builder::add_element(const element& element) -> layout_builder&
{
    elements.push_back(element);
    return *this;
}

auto layout_builder::end() -> layout_builder&
{
    layout finished = std::move(layouts.top());
    layouts.pop();

    if (!layouts.empty())
    {
        auto& parent = layouts.top();
        const bool vertical = parent.format.direction == direction::VERTICAL;

        float total = 0.0f;
        if (vertical)
        {
            total = finished.cursor.y + finished.format.padding.bottom + finished.format.padding.top;
            parent.cursor.y += total;
        }
        else
        {
            total = finished.cursor.x + finished.format.padding.left + finished.format.padding.right;
            parent.cursor.x += total;
        }
    }

    return *this;
}

}