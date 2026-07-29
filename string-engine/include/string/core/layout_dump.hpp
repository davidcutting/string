#pragma once

#include <cstdio>
#include <string>
#include <string_view>

#include <string/core/layout.hpp>

// Layout-tree dump (brief 12, M0a) — the MECHANICAL gate for the UI facade migration.
//
// Brief 11's bold refactors were affordable because `AE=0` + sync-validation made them cheap to
// VERIFY. The UI has no such gate: brief-05 screen parity is *visual*, so every iteration costs the
// user's eyes. This is the `AE=0`-shaped stand-in for the layers a screenshot cannot cheaply police
// (L0 layout -> L4 facade): it serialises the positioned tree — ids, resolved boxes, sizing
// resolution, format, visuals and text — as stable, diffable text.
//
// What it CATCHES: silently-changed sizing resolution, id assignment/collision, tree structure,
// padding/gap/alignment drift, text routing. That bug class is murder to spot by eye and trivial to
// spot in a diff.
// What it does NOT catch: motion, docking feel, theme *look*. Those stay eyes-on — the dump is a
// complement to visual verify, not a replacement.
//
// DETERMINISM CONTRACT: the dump is only diffable if the frame is. Authors that show a frame
// counter, wall-clock time or dt-driven animation (the brief-05 status panel and action-bar
// cooldowns both do) must be captured at a FIXED frame with STRING_FIXED_DT set — the same
// discipline the render gates use. Dump the same frame index on both sides of a change.
namespace string
{

namespace detail
{

inline void dump_append_escaped(std::string& out, std::string_view s)
{
    for (const char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            // Escape control bytes so a stray glyph can never break line-per-node diffing. UTF-8
            // continuation bytes (>= 0x80) pass through verbatim — player names are Unicode.
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned char>(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
}

inline const char* dump_mode(size_mode m)
{
    switch (m)
    {
    case size_mode::FIT:   return "fit";
    case size_mode::FIXED: return "fixed";
    case size_mode::GROW:  return "grow";
    }
    return "?";
}

inline const char* dump_dir(direction d)
{
    return d == direction::HORIZONTAL ? "H" : "V";
}

inline const char* dump_align(alignment a)
{
    switch (a)
    {
    case alignment::TOP:    return "top";
    case alignment::LEFT:   return "left";
    case alignment::CENTER: return "center";
    case alignment::RIGHT:  return "right";
    case alignment::BOTTOM: return "bottom";
    }
    return "?";
}

inline const char* dump_justify(justification j)
{
    switch (j)
    {
    case justification::START:         return "start";
    case justification::CENTER:        return "center";
    case justification::END:           return "end";
    case justification::SPACE_BETWEEN: return "between";
    case justification::SPACE_AROUND:  return "around";
    case justification::SPACE_EVENLY:  return "evenly";
    }
    return "?";
}

inline const char* dump_shape(shape s)
{
    switch (s)
    {
    case shape::RECTANGLE:         return "rect";
    case shape::ROUNDED_RECTANGLE: return "round";
    case shape::CIRCLE:            return "circle";
    }
    return "?";
}

inline void dump_axis(std::string& out, const axis_sizing& a)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s(%u,%u..%u)", dump_mode(a.mode),
                  static_cast<unsigned>(a.value), static_cast<unsigned>(a.min),
                  static_cast<unsigned>(a.max));
    out += buf;
}

inline void dump_color(std::string& out, const color& c)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    out += buf;
}

}  // namespace detail

// Serialise the positioned tree as stable text: a header, then ONE LINE PER NODE in painter
// (pre-order) order, fields always present and always in the same order so a diff points at the
// field that changed rather than at a reflowed blob.
//
// `label` is echoed into the header (screen name, milestone, whatever the harness wants) — it does
// not affect the node lines, so two dumps of the same tree under different labels differ only on
// that line.
[[nodiscard]] inline std::string dump_layout(const layout_builder& builder,
                                             std::string_view label = {})
{
    const std::span<const layout_node> nodes = builder.nodes();
    const std::span<const text_run> texts = builder.text_runs();

    std::string out;
    out.reserve(nodes.size() * 200 + 128);

    out += "# string layout dump v1\n";
    if (!label.empty())
    {
        out += "# label ";
        detail::dump_append_escaped(out, label);
        out += '\n';
    }
    {
        char buf[96];
        const bounding_box root = nodes.empty() ? bounding_box{} : nodes.front().box;
        std::snprintf(buf, sizeof buf, "# nodes %zu root %ux%u\n", nodes.size(),
                      static_cast<unsigned>(root.dimension.width),
                      static_cast<unsigned>(root.dimension.height));
        out += buf;
    }

    char buf[256];
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const layout_node& n = nodes[i];

        // Depth by walking the parent chain. The pool is pre-order after layout, so this is
        // shallow — and walking (rather than trusting parent < i) keeps the dump correct even if
        // the pool's ordering guarantee is ever relaxed.
        unsigned depth = 0;
        for (uint32_t p = n.parent; p != tree_node::none && depth < nodes.size(); ++depth)
            p = nodes[p].parent;

        std::snprintf(buf, sizeof buf, "%4zu %*s", i, static_cast<int>(depth * 2), "");
        out += buf;

        // Identity. An id-less node dumps as `-` with hash 0: id-less nodes are non-interactive by
        // construction (hit_test resolves through them), so losing/gaining one is a real diff.
        out += "id=";
        if (n.element.id.name.empty())
            out += '-';
        else
            detail::dump_append_escaped(out, n.element.id.name);
        std::snprintf(buf, sizeof buf, "#%016llx",
                      static_cast<unsigned long long>(n.element.id.hash));
        out += buf;

        // Resolved geometry — the single most valuable field: this is what "sizing resolution
        // silently changed" looks like in a diff.
        std::snprintf(buf, sizeof buf, " box=%u,%u,%ux%u", static_cast<unsigned>(n.box.x),
                      static_cast<unsigned>(n.box.y),
                      static_cast<unsigned>(n.box.dimension.width),
                      static_cast<unsigned>(n.box.dimension.height));
        out += buf;

        out += " w=";
        detail::dump_axis(out, n.element.sizing.width);
        out += " h=";
        detail::dump_axis(out, n.element.sizing.height);

        // Container format. Dumped for leaves too (a leaf's format is inert but not meaningless —
        // it becomes live the moment the node gains a child).
        std::snprintf(buf, sizeof buf, " fmt=%s/%s/%s gap=%u pad=%u,%u,%u,%u",
                      detail::dump_dir(n.format.direction),
                      detail::dump_align(n.format.alignment),
                      detail::dump_justify(n.format.justify),
                      static_cast<unsigned>(n.format.gap),
                      static_cast<unsigned>(n.format.padding.left),
                      static_cast<unsigned>(n.format.padding.right),
                      static_cast<unsigned>(n.format.padding.top),
                      static_cast<unsigned>(n.format.padding.bottom));
        out += buf;

        // Visuals.
        out += " fill=";
        detail::dump_color(out, n.element.color);
        out += " stroke=";
        detail::dump_color(out, n.element.stroke_color);
        std::snprintf(buf, sizeof buf, " shape=%s r=%u sw=%u",
                      detail::dump_shape(n.element.shape),
                      static_cast<unsigned>(n.element.radius),
                      static_cast<unsigned>(n.element.stroke_width));
        out += buf;

        // Flags that change placement or draw order.
        if (n.element.floating)
        {
            std::snprintf(buf, sizeof buf, " float=%u,%u",
                          static_cast<unsigned>(n.element.float_x),
                          static_cast<unsigned>(n.element.float_y));
            out += buf;
        }
        else
        {
            out += " float=-";
        }
        out += n.element.overlay ? " overlay=1" : " overlay=-";
        if (n.element.sweep != 0)
        {
            std::snprintf(buf, sizeof buf, " sweep=%u", static_cast<unsigned>(n.element.sweep));
            out += buf;
        }

        // Text routing: the index is deliberately NOT dumped (it is an allocation order artifact
        // that a facade rewrite may legitimately renumber); the resolved content is what matters.
        if (n.element.text != 0 && n.element.text < texts.size())
        {
            const text_run& t = texts[n.element.text];
            out += " text=\"";
            detail::dump_append_escaped(out, t.str);
            std::snprintf(buf, sizeof buf, "\" px=%u", static_cast<unsigned>(t.font_px));
            out += buf;
        }

        out += '\n';
    }

    return out;
}

}  // namespace string
