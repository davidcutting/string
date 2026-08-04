#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// The text-editing model: caret, selection, and the keys that move them.
//
// SPLIT OUT FROM THE WIDGET DELIBERATELY. Editing is the one part of a text field that is pure logic
// over a string — no layout, no theme, no device — and it is also the part with all the edge cases
// (UTF-8 boundaries, word jumps, what a selection does to backspace, what typing over a selection
// means). Keeping it here makes every one of those directly unit-testable, which the widget's
// emit-into-a-tree shape does not.
//
// Byte offsets, not codepoint indices: the string is UTF-8 and every consumer (insert, erase,
// substr, the renderer's split) wants bytes. The invariant is that offsets always sit on a codepoint
// BOUNDARY, which the move helpers and `clamp` maintain.
namespace string::ui
{

// Codepoint-aware stepping. Each answers "the boundary before/after `i`", clamped to the string.
[[nodiscard]] std::size_t prev_codepoint(std::string_view s, std::size_t i) noexcept;
[[nodiscard]] std::size_t next_codepoint(std::string_view s, std::size_t i) noexcept;
// Word stepping, for Ctrl+Left / Ctrl+Right. Non-ASCII bytes count as word characters: a real word
// segmenter needs Unicode break tables, and treating every accented or CJK character as a separator
// would be far more wrong than treating them all as letters.
[[nodiscard]] std::size_t prev_word(std::string_view s, std::size_t i) noexcept;
[[nodiscard]] std::size_t next_word(std::string_view s, std::size_t i) noexcept;

// Caret plus selection anchor. When they are equal there is no selection and the caret is just a
// position — which is why this is two offsets rather than a position and a length: a length cannot
// say which END the user is dragging, and shift-arrow has to extend from the far end.
struct text_edit_state
{
    std::size_t caret = 0;
    std::size_t anchor = 0;

    [[nodiscard]] bool has_selection() const noexcept { return caret != anchor; }
    [[nodiscard]] std::size_t selection_begin() const noexcept { return caret < anchor ? caret : anchor; }
    [[nodiscard]] std::size_t selection_end() const noexcept { return caret < anchor ? anchor : caret; }

    // Pull both offsets back into `s` and onto codepoint boundaries. Call whenever the text may have
    // changed underneath — a bound value can be written by something other than this widget.
    void clamp(std::string_view s) noexcept;
};

// One frame's editing input, already de-edged (and repeat-expanded) by the host.
struct text_edit_keys
{
    std::string_view typed{};
    bool backspace = false;
    bool del = false;
    bool left = false;
    bool right = false;
    bool home = false;
    bool end = false;
    bool select = false;     // shift: extend the selection instead of collapsing it
    bool word = false;       // ctrl: move/delete by word
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool select_all = false;
    std::string_view clipboard{};
};

// Applies `k` to `text`/`st`. Returns whether the TEXT changed (a pure caret move does not).
//
// `copied`, when non-null, receives the text a copy or cut exported — the caller hands that to
// whatever owns the system clipboard, because this layer has no idea one exists.
bool apply_text_edit(std::string& text, text_edit_state& st, const text_edit_keys& k,
                     std::string* copied = nullptr);

}  // namespace string::ui
