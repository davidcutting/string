#include <string/ui/theme.hpp>

namespace string::ui
{
namespace
{
// Non-owning. The app sets this to an instance it keeps alive for the process; until then the
// kit's own defaults answer, so nothing has to be initialised before a panel can be authored.
const Theme* g_theme = nullptr;

const Theme& fallback()
{
    static const Theme t{};
    return t;
}
}  // namespace

void set_theme(const Theme& t) { g_theme = &t; }

const Theme& theme() { return g_theme ? *g_theme : fallback(); }

}  // namespace string::ui
