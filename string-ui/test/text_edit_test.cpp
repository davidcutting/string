#include <gtest/gtest.h>

#include <string/ui/text_edit.hpp>

using namespace string::ui;

namespace
{
// "abé" — the last character is two bytes. Used throughout, because every offset rule in this file
// is really a UTF-8 rule and an ASCII-only fixture would prove none of them.
constexpr const char* kAccented = "ab\xC3\xA9";

text_edit_keys typed(std::string_view s)
{
    text_edit_keys k;
    k.typed = s;
    return k;
}
}  // namespace

// --- Codepoint stepping -----------------------------------------------------------------------

TEST(TextEdit, StepsWholeCodepointsNotBytes)
{
    const std::string_view s = kAccented;   // a=0, b=1, é=2..3, end=4
    EXPECT_EQ(next_codepoint(s, 0), 1u);
    EXPECT_EQ(next_codepoint(s, 2), 4u) << "the 2-byte character is one step";
    EXPECT_EQ(prev_codepoint(s, 4), 2u);
    EXPECT_EQ(prev_codepoint(s, 2), 1u);
    // Clamped at both ends rather than wrapping or running off.
    EXPECT_EQ(prev_codepoint(s, 0), 0u);
    EXPECT_EQ(next_codepoint(s, 4), 4u);
}

TEST(TextEdit, WordStepsSkipSeparatorsThenTheWord)
{
    const std::string_view s = "one two  three";
    EXPECT_EQ(next_word(s, 0), 4u);           // past "one" and its space
    EXPECT_EQ(next_word(s, 4), 9u);           // past "two" and BOTH spaces
    EXPECT_EQ(prev_word(s, 14), 9u);
    // From just inside a word, back to its start — not to the previous word.
    EXPECT_EQ(prev_word(s, 6), 4u);
    // At the very start of a word, the leading separators must be crossed first, or Ctrl+Left
    // would stall one character back forever.
    EXPECT_EQ(prev_word(s, 4), 0u);
}

// --- Insertion and deletion --------------------------------------------------------------------

TEST(TextEdit, TypingInsertsAtTheCaretNotTheEnd)
{
    std::string t = "ac";
    text_edit_state st{ 1, 1 };
    EXPECT_TRUE(apply_text_edit(t, st, typed("b")));
    EXPECT_EQ(t, "abc");
    EXPECT_EQ(st.caret, 2u) << "the caret follows what was typed";
}

TEST(TextEdit, BackspaceRemovesAWholeCodepoint)
{
    std::string t = kAccented;
    text_edit_state st{ t.size(), t.size() };
    text_edit_keys k; k.backspace = true;
    EXPECT_TRUE(apply_text_edit(t, st, k));
    EXPECT_EQ(t, "ab") << "a byte-wise erase would leave a dangling lead byte";
}

TEST(TextEdit, ForwardDeleteRemovesTheCodepointAfterTheCaret)
{
    std::string t = kAccented;
    text_edit_state st{ 2, 2 };
    text_edit_keys k; k.del = true;
    EXPECT_TRUE(apply_text_edit(t, st, k));
    EXPECT_EQ(t, "ab");
    EXPECT_EQ(st.caret, 2u) << "forward delete does not move the caret";
}

TEST(TextEdit, DeletingAtTheEdgesIsSafe)
{
    std::string t;
    text_edit_state st{};
    text_edit_keys back; back.backspace = true;
    text_edit_keys fwd;  fwd.del = true;
    EXPECT_FALSE(apply_text_edit(t, st, back));
    EXPECT_FALSE(apply_text_edit(t, st, fwd));
    EXPECT_EQ(t, "");
}

// --- Selection ----------------------------------------------------------------------------------

TEST(TextEdit, ShiftArrowExtendsAndPlainArrowCollapses)
{
    std::string t = "abcd";
    text_edit_state st{ 0, 0 };

    text_edit_keys sel_right; sel_right.right = true; sel_right.select = true;
    apply_text_edit(t, st, sel_right);
    apply_text_edit(t, st, sel_right);
    EXPECT_TRUE(st.has_selection());
    EXPECT_EQ(st.anchor, 0u);
    EXPECT_EQ(st.caret, 2u);

    // Plain Right with a selection collapses to its FAR edge rather than stepping from the caret.
    text_edit_keys right; right.right = true;
    apply_text_edit(t, st, right);
    EXPECT_FALSE(st.has_selection());
    EXPECT_EQ(st.caret, 2u);
}

TEST(TextEdit, TypingReplacesTheSelection)
{
    std::string t = "abcd";
    text_edit_state st{ 3, 1 };   // "bc" selected, caret at the right
    EXPECT_TRUE(apply_text_edit(t, st, typed("X")));
    EXPECT_EQ(t, "aXd");
    EXPECT_EQ(st.caret, 2u);
    EXPECT_FALSE(st.has_selection());
}

// Both deletion keys delete the SELECTION when there is one — treating backspace as "one character
// back" while text is highlighted loses the selection silently, which no text field does.
TEST(TextEdit, BothDeleteKeysRemoveTheSelection)
{
    for (int which = 0; which < 2; ++which)
    {
        std::string t = "abcd";
        text_edit_state st{ 1, 3 };
        text_edit_keys k;
        (which == 0 ? k.backspace : k.del) = true;
        EXPECT_TRUE(apply_text_edit(t, st, k));
        EXPECT_EQ(t, "ad");
        EXPECT_EQ(st.caret, 1u);
        EXPECT_FALSE(st.has_selection());
    }
}

TEST(TextEdit, SelectAllSpansTheWholeString)
{
    std::string t = kAccented;
    text_edit_state st{ 1, 1 };
    text_edit_keys k; k.select_all = true;
    apply_text_edit(t, st, k);
    EXPECT_EQ(st.selection_begin(), 0u);
    EXPECT_EQ(st.selection_end(), t.size());
}

// --- Clipboard ------------------------------------------------------------------------------------

TEST(TextEdit, CopyExportsTheSelectionAndLeavesTheTextAlone)
{
    std::string t = "abcd";
    text_edit_state st{ 1, 3 };
    text_edit_keys k; k.copy = true;
    std::string out;
    EXPECT_FALSE(apply_text_edit(t, st, k, &out)) << "a copy does not change the text";
    EXPECT_EQ(out, "bc");
    EXPECT_EQ(t, "abcd");
}

TEST(TextEdit, CutExportsAndRemoves)
{
    std::string t = "abcd";
    text_edit_state st{ 1, 3 };
    text_edit_keys k; k.cut = true;
    std::string out;
    EXPECT_TRUE(apply_text_edit(t, st, k, &out));
    EXPECT_EQ(out, "bc");
    EXPECT_EQ(t, "ad");
}

// With nothing selected, copy/cut are NO-OPS rather than acting on the whole field — silently
// clearing a line because nothing was highlighted is a surprising way to lose work.
TEST(TextEdit, CopyAndCutWithoutASelectionDoNothing)
{
    std::string t = "abcd";
    text_edit_state st{ 2, 2 };
    text_edit_keys k; k.cut = true;
    std::string out;
    EXPECT_FALSE(apply_text_edit(t, st, k, &out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(t, "abcd");
}

TEST(TextEdit, PasteReplacesTheSelectionAndAdvancesTheCaret)
{
    std::string t = "abcd";
    text_edit_state st{ 1, 3 };
    text_edit_keys k; k.paste = true; k.clipboard = "XY";
    EXPECT_TRUE(apply_text_edit(t, st, k));
    EXPECT_EQ(t, "aXYd");
    EXPECT_EQ(st.caret, 3u);
}

// --- Robustness -----------------------------------------------------------------------------------

// The bound value can be written by something other than this widget — another binding, an undo, a
// console command. A caret left pointing past the end would then index out of the string.
TEST(TextEdit, ClampsWhenTheTextShrinksUnderneath)
{
    std::string t = "abcdef";
    text_edit_state st{ 6, 5 };
    t = "ab";
    st.clamp(t);
    EXPECT_EQ(st.caret, 2u);
    EXPECT_EQ(st.anchor, 2u);
}

TEST(TextEdit, ClampNeverLandsInsideACodepoint)
{
    const std::string t = kAccented;
    text_edit_state st{ 3, 3 };   // mid-way through the 2-byte character
    st.clamp(t);
    EXPECT_EQ(st.caret, 2u);
    EXPECT_EQ(st.anchor, 2u);
}

// A frame can carry both a caret key and a character (fast typing, or a repeat landing alongside
// input). The move must apply first, or the character lands where the caret USED to be.
TEST(TextEdit, AMoveAndATypeInOneFrameApplyInOrder)
{
    std::string t = "ac";
    text_edit_state st{ 2, 2 };
    text_edit_keys k = typed("b");
    k.left = true;
    apply_text_edit(t, st, k);
    EXPECT_EQ(t, "abc");
}

// Select-all and copy can land in the same frame (both are Ctrl chords, and a slow frame can carry
// two edges). "Select all, then copy" is the only reading of that pair anyone means, so the order
// inside one frame is not arbitrary.
TEST(TextEdit, SelectAllAppliesBeforeACopyInTheSameFrame)
{
    std::string t = "abcd";
    text_edit_state st{ 2, 2 };
    text_edit_keys k; k.select_all = true; k.copy = true;
    std::string out;
    apply_text_edit(t, st, k, &out);
    EXPECT_EQ(out, "abcd");
}
