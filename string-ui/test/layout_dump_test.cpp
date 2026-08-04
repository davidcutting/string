#include <gtest/gtest.h>

#include <string>

#include <string/ui/layout.hpp>
#include <string/ui/layout_dump.hpp>

using namespace string;
using namespace string::literals;

namespace
{
// Count how many node lines a dump has (every line after the '#' header block is one node).
std::size_t node_lines(const std::string& dump)
{
    std::size_t n = 0;
    std::size_t pos = 0;
    while (pos < dump.size())
    {
        const std::size_t eol = dump.find('\n', pos);
        const std::string_view line{ dump.data() + pos, (eol == std::string::npos ? dump.size() : eol) - pos };
        if (!line.empty() && line.front() != '#') ++n;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return n;
}

// A small representative tree: a sized container with an id'd child and a text leaf.
void author(layout_builder& b)
{
    element panel{};
    panel.id = make_id("panel");
    panel.color = { 26, 28, 42, 235 };
    panel.stroke_color = { 70, 74, 100, 255 };
    panel.radius = 12;
    panel.stroke_width = 1;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fixed(200), fit() };

    b.begin(panel, format{ .padding = { 8, 8, 6, 6 }, .gap = 4,
                           .alignment = alignment::CENTER,
                           .direction = direction::VERTICAL });
    element row{};
    row.id = make_id("row");
    row.sizing = { grow(), fixed(20) };
    b.add_element(row);
    element label{};
    label.color = { 205, 214, 244, 255 };
    label.sizing = size_fit();
    b.add_text(label, "Hello", 22);
    b.end(dimension{ 800, 600 });
}
}  // namespace

// The dump must be a pure function of the tree: same tree -> byte-identical text. This is the whole
// premise of the M0a gate (diff two dumps, trust the diff).
TEST(LayoutDumpTest, IsDeterministicForTheSameTree)
{
    layout_builder a;
    layout_builder c;
    author(a);
    author(c);
    EXPECT_EQ(dump_layout(a), dump_layout(c));
}

// One line per node, in painter (pre-order) order, plus the header block.
TEST(LayoutDumpTest, EmitsOneLinePerNode)
{
    layout_builder b;
    author(b);
    const std::string dump = dump_layout(b);
    EXPECT_EQ(node_lines(dump), b.nodes().size());
    EXPECT_NE(dump.find("# string layout dump v1"), std::string::npos);
}

// The fields the gate exists to police: resolved box, sizing resolution, ids, text content.
TEST(LayoutDumpTest, RecordsResolvedGeometryIdsAndText)
{
    layout_builder b;
    author(b);
    const std::string dump = dump_layout(b);

    EXPECT_NE(dump.find("id=panel#"), std::string::npos);
    EXPECT_NE(dump.find("id=row#"), std::string::npos);
    EXPECT_NE(dump.find("w=fixed(200,0..65535)"), std::string::npos);  // sizing resolution
    EXPECT_NE(dump.find("text=\"Hello\" px=22"), std::string::npos);   // text routing
    EXPECT_NE(dump.find("fmt=V/center/start gap=4 pad=8,8,6,6"), std::string::npos);
    EXPECT_NE(dump.find("shape=round r=12 sw=1"), std::string::npos);
    // An id-less node (the text leaf) is still enumerated — losing/gaining one is a real diff.
    EXPECT_NE(dump.find("id=-#0000000000000000"), std::string::npos);
}

// A changed resolved box MUST show up — otherwise the gate is decorative. This is the exact bug
// class M0a exists for: the tree "looks the same" but sizing resolved differently.
TEST(LayoutDumpTest, DetectsChangedSizingResolution)
{
    layout_builder before;
    author(before);

    layout_builder after;
    {
        element panel{};
        panel.id = make_id("panel");
        panel.color = { 26, 28, 42, 235 };
        panel.stroke_color = { 70, 74, 100, 255 };
        panel.radius = 12;
        panel.stroke_width = 1;
        panel.shape = shape::ROUNDED_RECTANGLE;
        panel.sizing = { fixed(240), fit() };   // 200 -> 240, nothing else touched
        after.begin(panel, format{ .padding = { 8, 8, 6, 6 }, .gap = 4,
                                   .alignment = alignment::CENTER,
                                   .direction = direction::VERTICAL });
        element row{};
        row.id = make_id("row");
        row.sizing = { grow(), fixed(20) };
        after.add_element(row);
        element label{};
        label.color = { 205, 214, 244, 255 };
        label.sizing = size_fit();
        after.add_text(label, "Hello", 22);
        after.end(dimension{ 800, 600 });
    }

    EXPECT_NE(dump_layout(before), dump_layout(after));
}

// Text is escaped so a newline or quote in author-supplied content (player names are arbitrary
// Unicode) can never break the one-line-per-node invariant the diff relies on.
TEST(LayoutDumpTest, EscapesTextSoLinesStayIntact)
{
    layout_builder b;
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{});
    element label{};
    label.sizing = size_fit();
    b.add_text(label, "a\"b\nc", 16);
    b.end(dimension{ 100, 100 });

    const std::string dump = dump_layout(b);
    EXPECT_NE(dump.find("text=\"a\\\"b\\nc\""), std::string::npos);
    EXPECT_EQ(node_lines(dump), b.nodes().size());   // the embedded newline did not split a line
}

// The label rides in the header only — two dumps of the same tree differ by exactly that one line.
TEST(LayoutDumpTest, LabelDoesNotAffectNodeLines)
{
    layout_builder b;
    author(b);
    const std::string bare = dump_layout(b);
    const std::string tagged = dump_layout(b, "inventory");
    EXPECT_NE(tagged.find("# label inventory"), std::string::npos);
    EXPECT_EQ(node_lines(bare), node_lines(tagged));
    EXPECT_EQ(bare.substr(bare.find("\n   ")), tagged.substr(tagged.find("\n   ")));
}
