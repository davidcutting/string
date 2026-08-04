#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <string/ui/frame_arena.hpp>

using namespace string::ui;

// THE invariant, and the reason the first version was a deque: a view handed out earlier must stay
// valid however much is appended after it. A vector<char> arena would break this on every regrowth
// and the symptom would be garbage glyphs, not a crash.
TEST(FrameArena, EarlierViewsSurviveAnyAmountOfLaterAppending)
{
    FrameArena a;
    std::vector<std::string_view> views;
    std::vector<std::string> expect;

    // Well past one block, so the arena is forced to add blocks while old views are outstanding.
    for (int i = 0; i < 20000; ++i)
    {
        expect.push_back("nameplate_" + std::to_string(i));
        views.push_back(a.own(expect.back()));
    }
    ASSERT_GT(a.block_count(), 1u) << "the test must actually cross a block boundary";

    for (std::size_t i = 0; i < views.size(); ++i)
        EXPECT_EQ(views[i], expect[i]) << "view " << i << " moved or was overwritten";
}

TEST(FrameArena, ClearReusesMemoryRatherThanFreeingIt)
{
    FrameArena a;
    for (int i = 0; i < 5000; ++i) (void)a.own("some text for the arena");
    const std::size_t blocks = a.block_count();
    EXPECT_GT(a.bytes_used(), 0u);

    a.clear();
    EXPECT_EQ(a.bytes_used(), 0u);
    EXPECT_EQ(a.block_count(), blocks) << "clear must keep the blocks — reusing them is the point";

    // The same volume again must not need new blocks.
    for (int i = 0; i < 5000; ++i) (void)a.own("some text for the arena");
    EXPECT_EQ(a.block_count(), blocks);
}

// A string larger than a block gets its own oversized block. Truncating or splitting would lose
// text, which is far worse than one outsized allocation.
TEST(FrameArena, HandlesAStringLargerThanABlock)
{
    FrameArena a;
    const std::string huge(200 * 1024, 'x');
    const std::string_view v = a.own(huge);
    EXPECT_EQ(v.size(), huge.size());
    EXPECT_EQ(v, huge);

    // ...and the arena still works normally afterwards.
    const std::string_view after = a.own("small");
    EXPECT_EQ(after, "small");
    EXPECT_EQ(v, huge) << "the oversized entry must not have been disturbed";
}

TEST(FrameArena, EmptyStringsAreFree)
{
    FrameArena a;
    EXPECT_TRUE(a.own("").empty());
    EXPECT_EQ(a.bytes_used(), 0u);
    EXPECT_EQ(a.block_count(), 0u) << "an empty string must not force a block to exist";
}

// Views must be independent copies: mutating the source afterwards cannot reach into the arena.
TEST(FrameArena, OwnCopiesRatherThanAliasing)
{
    FrameArena a;
    std::string src = "original";
    const std::string_view v = a.own(src);
    src = "clobbered";
    EXPECT_EQ(v, "original");
}
