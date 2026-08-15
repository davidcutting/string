#include <string/render/render_debug.hpp>

#include <algorithm>
#include <cstdio>
#include <string>

#include <string/debug_draw.hpp>
#include <string/ui/theme.hpp>
#include <string/ui/widgets.hpp>

#include <string/render/render_cvars.hpp>
#include <string/scene/world.hpp>

namespace string::render
{

using ::string::ui::theme;
using ::string::grow;
using ::string::make_id;

void RenderDebug::hud_rows(::string::ui::Ui& p, const MeshOverlayStats* mesh_stats) const
{
    // Consolidated mesh/cull stats (formerly the standalone overlay in demo_scene.cpp): ONE
    // stats surface. Kept informative — the same numbers agents used to grep from it.
    if (!mesh_stats) return;

    const GpuMeshStats& s = mesh_stats->stats;
    static const char* view_names[] = { "off", "meshlet", "LOD", "occlusion" };
    auto pct = [](uint32_t num, uint32_t den) {
        return den == 0 ? std::string("--") : std::to_string(num * 100 / den) + "%";
    };
    const auto add = [&](std::string t) {
        p.text(p.own(std::move(t))).color(theme().text_dim).font(15).width(grow());
    };
    add("HiZ " + std::string(mesh_stats->hiz_enabled ? "on" : "off")
        + "  view " + view_names[mesh_stats->debug_view & 3]
        + (mesh_stats->crowd_enabled ? "  [CROWD]" : ""));
    add("meshlets " + std::to_string(s.meshlets_total) + " / "
        + std::to_string(mesh_stats->total_meshlets));
    add("frustum " + std::to_string(s.after_frustum) + " (" + pct(s.after_frustum, s.meshlets_total) + ")");
    add("cone    " + std::to_string(s.after_cone) + " (" + pct(s.after_cone, s.meshlets_total) + ")");
    add("drawn   " + std::to_string(s.after_hiz) + " (" + pct(s.after_hiz, s.meshlets_total) + ")");
    add("draws " + std::to_string(mesh_stats->draw_count) + "  lights "
        + std::to_string(mesh_stats->lights.size()));
}

// The ENTITY list, enumerated from the world (brief 18/S8: the scene layer is the authority on
// what exists; the draw table below is the renderer's derived view of it).
static void entity_rows(::string::ui::Ui& p, const ::string::scene::world& world)
{
    using namespace ::string::ui;
    const std::span<const ::string::scene::entity> live = world.entities();
    p.text(p.own("entities: " + std::to_string(live.size()))).color(theme().text_dim).font(14);
    const int emax = std::min<int>(8, static_cast<int>(live.size()));
    for (int i = 0; i < emax; ++i)
    {
        const ::string::scene::entity e = live[i];
        const std::string_view name = world.name_of(e);
        const uint64_t sid = world.server_id_of(e);
        std::string row = "e" + std::to_string(e.index) + " "
                        + (name.empty() ? std::string("(unnamed)") : std::string(name));
        if (sid != 0) row += " sid=" + std::to_string(sid);
        p.text(p.own(std::move(row))).color(theme().text).font(13).width(grow());
    }
    if (static_cast<int>(live.size()) > emax)
        p.text(p.own("… +" + std::to_string(live.size() - emax) + " more"))
         .color(theme().text_dim).font(13);
}

// Default rect, named because BOTH the panel and its row-budget calculation need it (the store is
// keyed on it, and reading the stored rect is how the table sizes itself to the panel). Must match
// the rect string-debug opens the inspector panel with.
static constexpr ::string::ui::panel_rect kInspectorRect{ 820.0f, 16.0f, 360.0f, 460.0f };

void RenderDebug::inspector(::string::ui::Ui& p, const MeshOverlayStats* mesh_stats,
                            const ::string::scene::world* world)
{
    using namespace ::string::ui;

    if (!mesh_stats || mesh_stats->draws.empty())
    {
        p.text("no scene loaded").color(theme().text_dim).font(15);
        return;
    }

    if (world != nullptr) entity_rows(p, *world);

    const int isolate = cv_isolate_draw().get();
    p.text(p.own("hover to highlight · wheel to scroll · isolate=" + std::to_string(isolate)))
     .color(theme().text_dim)
     .font(14);

    // VIRTUALIZED, not a scroll area: the rows are uniform and a scene can have thousands of
    // draws, so building only the visible ones is the whole point (brief 13's lever on
    // immediate-mode cost). A scroll_area would build every row to then clip most of them.
    //
    // The row budget follows the PANEL HEIGHT so resizing shows more rows rather than
    // scrolling a fixed window inside a bigger box. Read from the store, so one frame stale
    // during an active resize drag — invisible at drag speed.
    static const table_column kCols[] = {
        { "idx", 40 }, { "name", 120 }, { "meshlets", 64 }, { "lod", 32 }, { "res", 30 } };
    const panel_state& st = p.panels().panel(make_id("dbg_inspector").hash, kInspectorRect);
    const auto rows = static_cast<std::size_t>(
        std::clamp((st.rect.height - 150.0f) / 22.0f, 3.0f, 60.0f));

    auto t = table(p, "dbg_draws");
    t.columns(kCols)
     .visible_rows(rows)
     .selected(bind(*this, &RenderDebug::selected_draw_))
     .on_hover([&](std::size_t r) {
         // Sweeping the list highlights each draw's bounds in-world. This is why the table
         // needed on_hover: waiting for a click would make the tool useless for scanning.
         const InspectorDraw& d = mesh_stats->draws[r];
         ::string::debug::aabb(d.aabb_min, d.aabb_max,
                               ::string::debug::Color{ 249, 226, 175, 255 });
     })
     .rows(mesh_stats->draws.size(), [&](Ui& c, std::size_t r, std::size_t col) {
         const InspectorDraw& d = mesh_stats->draws[r];
         char buf[64];
         switch (col)
         {
             case 0: std::snprintf(buf, sizeof(buf), "%u", d.index); break;
             case 1: std::snprintf(buf, sizeof(buf), "%s", d.name.c_str()); break;
             case 2: std::snprintf(buf, sizeof(buf), "%u", d.meshlet_count); break;
             case 3: std::snprintf(buf, sizeof(buf), "%u", d.lod_count); break;
             default: std::snprintf(buf, sizeof(buf), "%s", d.resident ? "Y" : "-"); break;
         }
         c.text(c.own(std::string(buf)))
          .color(static_cast<int>(r) == isolate ? theme().accent_warm : theme().text)
          .font(14);
     });

    // The SELECTED draw stays highlighted after the pointer leaves, in a different colour —
    // that is the difference between "what am I pointing at" and "what am I working on".
    if (selected_draw_ >= 0 && selected_draw_ < int(mesh_stats->draws.size()))
    {
        const InspectorDraw& d = mesh_stats->draws[selected_draw_];
        ::string::debug::aabb(d.aabb_min, d.aabb_max,
                              ::string::debug::Color{ 137, 180, 250, 255 });
    }

    // Lights list header + a few rows.
    p.text(p.own("lights: " + std::to_string(mesh_stats->lights.size())))
     .color(theme().text_dim)
     .font(14);
    const int lmax = std::min<int>(6, int(mesh_stats->lights.size()));
    for (int i = 0; i < lmax; ++i)
    {
        const InspectorLight& L = mesh_stats->lights[i];
        char buf[128];
        std::snprintf(buf, sizeof(buf), "L%-2d (%.1f,%.1f,%.1f) r=%.1f %s",
                      i, L.position.x, L.position.y, L.position.z, L.range,
                      L.spot ? "spot" : "point");
        p.text(p.own(std::string(buf))).color(theme().text).font(13).width(grow());
        // Mark each light with a small overlay sphere so it's locatable in-world.
        ::string::debug::sphere_overlay(L.position, 0.15f,
                                        ::string::debug::Color{ 249, 226, 175, 255 }, 10);
    }
}

}  // namespace string::render
