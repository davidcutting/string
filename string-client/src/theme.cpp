#include <string/client/theme.hpp>

namespace string::client
{

const ::string::ui::Theme& theme()
{
    // The engine's defaults ARE the brief-05 palette (they were lifted from it when the type was
    // promoted), so this instance carries the same surface/content colours the constants did — the
    // swap is deliberately value-neutral, which is what lets the layout-dump gate police it.
    // Metrics and motion come from the same instance now too, so a future retheme is one edit here
    // rather than a hunt through screen code.
    static const ::string::ui::Theme t{};
    return t;
}

}  // namespace string::client
