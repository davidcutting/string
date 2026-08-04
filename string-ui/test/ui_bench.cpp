#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <string/ui/text_measurer.hpp>
#include <string/ui/widgets.hpp>

// Brief 12b's benchmark harness — the baseline every milestone in that brief reports against.
//
// A REPORT, NOT A GATE. There are no thresholds: this runs on whatever machine the build lands on,
// and a wall-clock assertion on shared hardware is a flake factory. It prints a table; the milestone
// records the delta in the brief. The one thing it asserts is that the scenarios actually built the
// trees they claim to, so a silently-empty benchmark cannot report a fantastic number.
//
// It exists in the repo because the numbers that justify brief 12b were previously not reproducible
// from the tree — the design rested on a table nobody else could regenerate.
//
// Runs the REAL stack: the fluent facade, real widgets and panels, and the real SDF measurer over a
// real font. The kit depends on nothing, so all of that is free headlessly — which is exactly why
// this can live in the unit-test target rather than needing a device.
using namespace string;
using namespace string::ui;

namespace
{

std::vector<std::uint8_t> read_font()
{
    std::ifstream f(STRING_FONT_TEST_PATH, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

constexpr dimension kScreen{ 2560, 1440 };
constexpr int kFrames = 200;   // enough to average out scheduler noise without a slow test

// Row labels are deliberately VARIED and non-trivial. A benchmark that shapes the same short string
// everywhere measures a cache that a real UI would miss on.
const char* row_text(std::size_t i)
{
    static const char* kNames[] = {
        "geometry.phase1", "hiz.build", "shadow.cascades", "froxel.cull", "gtao",
        "probe relight", "transparency", "composite", "ui overlay", "post bloom",
        "meshlet expand", "draw cull",
    };
    return kNames[i % 12];
}

struct Timing
{
    double author_us = 0;
    double layout_us = 0;
    std::size_t nodes = 0;
    double surfaces = 0;   // per frame
    double skipped = 0;    // ...of which restored from the retained store (12b M3)
};

// One scenario: `author` builds the tree through the facade, then it is laid out with the real
// measurer. Author and layout are timed separately because they are different products — brief 12b's
// whole point is that layout (i.e. shaping) dominates, and a combined number would hide it.
template <typename Author>
Timing run(dynamic_font_atlas& atlas, text_shape_cache* cache, Author&& author, bool skip = false)
{
    layout_builder builder;
    // EXPLICIT in both arms. The skip defaults on, so leaving it implicit would fold M3's win into
    // the shaping-cache column and quietly invalidate M0's numbers.
    builder.set_skip_enabled(skip);
    interaction state{};
    state.screen = kScreen;
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };

    Timing t;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const auto a0 = std::chrono::steady_clock::now();
        ui.begin_frame();
        builder.clear();
        builder.begin(format{ .direction = direction::VERTICAL });
        author(ui, frame);
        const auto a1 = std::chrono::steady_clock::now();

        builder.end(kScreen, dynamic_text_measurer{ &atlas, builder.text_runs(), 0, cache });
        const auto a2 = std::chrono::steady_clock::now();

        ui.end_frame();
        if (cache != nullptr) cache->end_frame();

        // Discard the first frames: they populate the glyph atlas and any cache, and measuring a
        // cold start as though it were steady state would flatter or damn the wrong thing.
        if (frame >= 20)
        {
            t.author_us += std::chrono::duration<double, std::micro>(a1 - a0).count();
            t.layout_us += std::chrono::duration<double, std::micro>(a2 - a1).count();
            t.surfaces += static_cast<double>(builder.surfaces().size());
            t.skipped += static_cast<double>(builder.skipped_count());
        }
        t.nodes = builder.nodes().size();
    }
    const double n = kFrames - 20;
    t.author_us /= n;
    t.layout_us /= n;
    t.surfaces /= n;
    t.skipped /= n;
    return t;
}

void panels_scenario(Ui& u, int panel_count, int rows)
{
    for (int p = 0; p < panel_count; ++p)
    {
        u.panel(u.own("bench_panel" + std::to_string(p)))
            .title("Passes")
            .initial({ 20.0f + 40.0f * static_cast<float>(p), 20.0f, 380.0f, 600.0f })
            .content([&](Ui& c) {
                for (int r = 0; r < rows; ++r)
                {
                    c.element().row().gap(8).width(grow()).content([&](Ui& q) {
                        q.text(row_text(static_cast<std::size_t>(r))).font(15).width(grow());
                        q.text(q.own(std::to_string(r) + ".00 ms")).font(15);
                    });
                }
            });
    }
}

void nameplates_scenario(Ui& u, int count, int frame)
{
    for (int i = 0; i < count; ++i)
    {
        // MOVING, every frame, the way a world-anchored plate actually behaves. This used to be
        // static, with a note that movement was M2's problem — now that M2 has landed, a static
        // plate would measure the one case the milestone did NOT have to solve. Text and font are
        // unchanged, so the shaping-cache column stays a fair comparison; only the placement moves.
        const int drift = (frame * 3 + i) % 200;
        u.element()
            .floating(20 + (i % 40) * 60 + drift, 40 + (i / 40) * 24)
            .column()
            .content([&](Ui& c) { c.text(c.own("Player" + std::to_string(i))).font(14); });
    }
}

void report(const char* name, const Timing& base, const Timing& cached, const Timing& skipped)
{
    // TOTAL, not the layout column alone. M3 moves work from layout into authoring (folding the
    // signature is not free), so a layout-only ratio would flatter it. The honest figure is what a
    // frame costs end to end.
    const double before = cached.author_us + cached.layout_us;
    const double after = skipped.author_us + skipped.layout_us;
    const double gain = after > 0.0 ? before / after : 0.0;
    std::printf("| %-34s | %5zu | %7.0fus | %7.0fus | %9.0fus | %9.0fus | %9.0fus | %5.2fx | %4.0f/%-4.0f |\n",
                name, skipped.nodes, cached.author_us, skipped.author_us, base.layout_us,
                cached.layout_us, skipped.layout_us, gain, skipped.skipped, skipped.surfaces);
}

}  // namespace

TEST(UiBench, Report)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty()) << "test font not found at " << STRING_FONT_TEST_PATH;

    std::printf("\n=== brief 12b UI benchmark (%dx%d, %d frames, first 20 discarded) ===\n",
                kScreen.width, kScreen.height, kFrames);
    std::printf("| %-34s | %5s | %9s | %9s | %11s | %11s | %11s | %6s | %-9s |\n", "scenario",
                "nodes", "author", "author+sig", "layout base", "+M0 cache", "+M3 skip",
                "total", "skip/surf");

    struct Case
    {
        const char* name;
        int panels, rows, plates;
    };
    const Case cases[] = {
        { "debug shell (3 panels x 12 rows)", 3, 12, 0 },
        { "heavy tooling (10 panels x 30 rows)", 10, 30, 0 },
        { "shell + 200 nameplates", 3, 12, 200 },
        { "HUD stress (1000 nameplates)", 3, 12, 1000 },
        { "extreme (10x60 rows + 2000 plates)", 10, 60, 2000 },
    };

    // EACH SCENARIO RUN BOTH WAYS. The delta is M0's whole result, so it is re-derived on every run
    // rather than being a number someone recorded once and nobody else can reproduce — which is the
    // exact problem this harness exists to fix.
    for (const Case& c : cases)
    {
        const auto author = [&](Ui& u, int frame) {
            panels_scenario(u, c.panels, c.rows);
            if (c.plates > 0) nameplates_scenario(u, c.plates, frame);
        };

        dynamic_font_atlas a1(ttf);
        const Timing base = run(a1, nullptr, author, /*skip=*/false);

        dynamic_font_atlas a2(ttf);
        text_shape_cache c2;
        const Timing cached = run(a2, &c2, author, /*skip=*/false);

        dynamic_font_atlas a3(ttf);
        text_shape_cache c3;
        const Timing skipped = run(a3, &c3, author, /*skip=*/true);

        report(c.name, base, cached, skipped);
        EXPECT_GT(skipped.nodes, 0u) << "an empty tree would report a wonderful and meaningless number";
    }

    // The structural floor: identical trees with no measurement at all. Brief 12b's central claim is
    // that the gap between this and the rows above IS text shaping — so the claim is re-derived on
    // every run rather than being a number in a document.
    std::printf("\n--- structural floor (no_measure: fit/flex/wrap/position only) ---\n");
    for (const Case& c : cases)
    {
        layout_builder builder;
        interaction state{};
        state.screen = kScreen;
        Motion motion;
        Theme theme{};
        PanelStore panels;
        Ui ui{ builder, state, motion, theme, panels };

        double layout_us = 0;
        std::size_t nodes = 0;
        for (int frame = 0; frame < kFrames; ++frame)
        {
            ui.begin_frame();
            builder.clear();
            builder.begin(format{ .direction = direction::VERTICAL });
            panels_scenario(ui, c.panels, c.rows);
            if (c.plates > 0) nameplates_scenario(ui, c.plates, frame);
            const auto t0 = std::chrono::steady_clock::now();
            builder.end(kScreen);
            const auto t1 = std::chrono::steady_clock::now();
            ui.end_frame();
            if (frame >= 20) layout_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            nodes = builder.nodes().size();
        }
        std::printf("| %-34s | %5zu | %9s | %7.0fus | %10s |\n", c.name, nodes, "-",
                    layout_us / (kFrames - 20), "-");
    }
    std::printf("\n");
}

// --- Where authoring time actually goes (2026-08-03) ---------------------------------------------
//
// M0 left authoring as the dominant cost (3061us vs 1843us of layout at the extreme case), and none
// of brief 12b's remaining milestones touch it. Before committing to M1-M3 as the plan, this asks
// WHAT in authoring costs that — the answer decides whether the ordering is right.
//
// A LADDER: each rung removes exactly one thing, so each difference names one cost. Run on the
// nameplate scenario because that is where the element volume is.
TEST(UiBench, AuthoringBreakdown)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());

    constexpr int kPlates = 2000;

    // Pre-built names, so the "prebuilt" rung pays no string construction at author time.
    std::vector<std::string> names;
    names.reserve(kPlates);
    for (int i = 0; i < kPlates; ++i) names.push_back("Player" + std::to_string(i));

    // Times ONLY the authoring half; every rung lays out identically afterwards so the comparison is
    // apples to apples.
    const auto author_only = [&](auto&& body) {
        layout_builder builder;
        interaction state{};
        state.screen = kScreen;
        Motion motion;
        Theme theme{};
        PanelStore panels;
        Ui ui{ builder, state, motion, theme, panels };
        dynamic_font_atlas atlas(ttf);
        text_shape_cache cache;

        double us = 0;
        for (int frame = 0; frame < kFrames; ++frame)
        {
            const auto t0 = std::chrono::steady_clock::now();
            ui.begin_frame();
            builder.clear();
            builder.begin(format{ .direction = direction::VERTICAL });
            body(ui);
            const auto t1 = std::chrono::steady_clock::now();
            builder.end(kScreen, dynamic_text_measurer{ &atlas, builder.text_runs(), 0, &cache });
            ui.end_frame();
            cache.end_frame();
            if (frame >= 20) us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        return us / (kFrames - 20);
    };

    const double computed = author_only([&](Ui& u) {
        for (int i = 0; i < kPlates; ++i)
            u.element().floating(20 + (i % 40) * 60, 40 + (i / 40) * 24).column().content(
                [&](Ui& c) { c.text(c.own("Player" + std::to_string(i))).font(14); });
    });
    const double prebuilt = author_only([&](Ui& u) {
        for (int i = 0; i < kPlates; ++i)
            u.element().floating(20 + (i % 40) * 60, 40 + (i / 40) * 24).column().content(
                [&](Ui& c) { c.text(names[static_cast<std::size_t>(i)]).font(14); });
    });
    const double literal = author_only([&](Ui& u) {
        for (int i = 0; i < kPlates; ++i)
            u.element().floating(20 + (i % 40) * 60, 40 + (i / 40) * 24).column().content(
                [&](Ui& c) { c.text("Player").font(14); });
    });
    // SAME NODE COUNT as the rungs above — a plain child element instead of a text one. Dropping
    // the child entirely would have compared 1 node per plate against 2, and the difference would
    // have measured node count rather than the text path. (It did, in the first draft.)
    const double no_text = author_only([&](Ui& u) {
        for (int i = 0; i < kPlates; ++i)
            u.element().floating(20 + (i % 40) * 60, 40 + (i / 40) * 24).column().content(
                [](Ui& c) { c.element().fixed(60, 14); });
    });

    // The floor: the same node count straight into the builder — no facade, no arena, no hashing.
    double raw_us = 0;
    {
        layout_builder builder;
        for (int frame = 0; frame < kFrames; ++frame)
        {
            const auto t0 = std::chrono::steady_clock::now();
            builder.clear();
            builder.begin(format{ .direction = direction::VERTICAL });
            for (int i = 0; i < kPlates; ++i)
            {
                element outer{};
                outer.floating = true;
                outer.float_x = static_cast<std::int16_t>(20 + (i % 40) * 60);
                outer.float_y = static_cast<std::int16_t>(40 + (i / 40) * 24);
                builder.begin(outer, format{ .direction = direction::VERTICAL });
                element t{};
                t.sizing = size_fit();
                builder.add_text(t, "Player", 14);
                builder.end();
            }
            const auto t1 = std::chrono::steady_clock::now();
            builder.end(kScreen);
            if (frame >= 20) raw_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        raw_us /= (kFrames - 20);
    }

    std::printf("\n=== authoring breakdown (%d nameplates, author phase only) ===\n", kPlates);
    std::printf("  own(concat + to_string)    %8.0fus\n", computed);
    std::printf("  prebuilt name              %8.0fus   (-%.0fus: building the string)\n", prebuilt,
                computed - prebuilt);
    std::printf("  literal name               %8.0fus   (-%.0fus: unique-string arena copy)\n",
                literal, prebuilt - literal);
    std::printf("  plain child, no text       %8.0fus   (-%.0fus: own() + the text table)\n",
                no_text, literal - no_text);
    std::printf("  raw builder, no facade     %8.0fus   (-%.0fus: the facade, same nodes)\n",
                raw_us, no_text - raw_us);
    std::printf("\n");

    EXPECT_GT(computed, 0.0);
}
