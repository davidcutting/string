#include <string/ui/text_edit.hpp>

#include <algorithm>

namespace string::ui
{
namespace
{

// UTF-8 continuation bytes are 10xxxxxx. Every caret move steps over WHOLE codepoints, because
// landing inside one produces a malformed string the glyph decoder renders as garbage — and the
// original TextField's backspace already had to know this. It is the same rule, applied everywhere
// rather than in one place.
bool is_continuation(char c) noexcept
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

bool is_word_char(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u != ' ' && u != '\t' && u != '\n');
}

}  // namespace

std::size_t prev_codepoint(std::string_view s, std::size_t i) noexcept
{
    if (i == 0) return 0;
    --i;
    while (i > 0 && is_continuation(s[i])) --i;
    return i;
}

std::size_t next_codepoint(std::string_view s, std::size_t i) noexcept
{
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && is_continuation(s[i])) ++i;
    return i;
}

std::size_t prev_word(std::string_view s, std::size_t i) noexcept
{
    // Skip the run of separators immediately behind the caret, THEN the word. Without the first
    // step, Ctrl+Left at the start of a word would stop one character back and never leave it.
    while (i > 0)
    {
        const std::size_t p = prev_codepoint(s, i);
        if (is_word_char(s[p])) break;
        i = p;
    }
    while (i > 0)
    {
        const std::size_t p = prev_codepoint(s, i);
        if (!is_word_char(s[p])) break;
        i = p;
    }
    return i;
}

std::size_t next_word(std::string_view s, std::size_t i) noexcept
{
    while (i < s.size() && is_word_char(s[i])) i = next_codepoint(s, i);
    while (i < s.size() && !is_word_char(s[i])) i = next_codepoint(s, i);
    return i;
}

void text_edit_state::clamp(std::string_view s) noexcept
{
    // The bound value can change UNDER the widget — a binding driven by something else, an undo, a
    // console command. Clamping every frame is what keeps the caret from pointing past the end of a
    // string it never saw change.
    caret = std::min(caret, s.size());
    anchor = std::min(anchor, s.size());
    // ...and off a continuation byte, for the same reason a move never lands on one.
    while (caret > 0 && caret < s.size() && is_continuation(s[caret])) --caret;
    while (anchor > 0 && anchor < s.size() && is_continuation(s[anchor])) --anchor;
}

bool apply_text_edit(std::string& text, text_edit_state& st, const text_edit_keys& k,
                     std::string* copied)
{
    st.clamp(text);
    bool changed = false;

    const auto sel_lo = [&] { return std::min(st.caret, st.anchor); };
    const auto sel_hi = [&] { return std::max(st.caret, st.anchor); };

    // Replacing the selection is the first step of EVERY insertion (typing, paste), so it lives in
    // one place. Typing over a selection replacing it is the behaviour every text field has.
    const auto erase_selection = [&] {
        if (!st.has_selection()) return false;
        const std::size_t lo = sel_lo(), hi = sel_hi();
        text.erase(lo, hi - lo);
        st.caret = st.anchor = lo;
        return true;
    };

    // --- Select-all FIRST, so a frame carrying both it and a copy exports what was just selected.
    // The two orderings are not equally defensible: "select all, then copy" is the only reading of
    // that pair anyone means.
    if (k.select_all)
    {
        st.anchor = 0;
        st.caret = text.size();
    }

    // --- Clipboard. Copy/cut export the selection; with none, they are no-ops rather than acting
    // on the whole field, which would be a surprising way to lose a line.
    if ((k.copy || k.cut) && st.has_selection() && copied != nullptr)
        *copied = text.substr(sel_lo(), sel_hi() - sel_lo());
    if (k.cut && st.has_selection())
        changed |= erase_selection();
    if (k.paste && !k.clipboard.empty())
    {
        erase_selection();
        text.insert(st.caret, k.clipboard);
        st.caret += k.clipboard.size();
        st.anchor = st.caret;
        changed = true;
    }

    if (k.select_all)
    {
        st.anchor = 0;
        st.caret = text.size();
    }

    // --- Deletion. With a selection, both keys delete it — that is what the selection is FOR, and
    // treating backspace as "one character back" while something is selected loses the selection
    // silently.
    if (k.backspace)
    {
        if (st.has_selection()) changed |= erase_selection();
        else if (st.caret > 0)
        {
            const std::size_t p = k.word ? prev_word(text, st.caret) : prev_codepoint(text, st.caret);
            text.erase(p, st.caret - p);
            st.caret = st.anchor = p;
            changed = true;
        }
    }
    if (k.del)
    {
        if (st.has_selection()) changed |= erase_selection();
        else if (st.caret < text.size())
        {
            const std::size_t n = k.word ? next_word(text, st.caret) : next_codepoint(text, st.caret);
            text.erase(st.caret, n - st.caret);
            changed = true;
        }
    }

    // --- Caret movement. `select` extends (anchor stays put); without it the anchor follows, which
    // IS how a selection collapses.
    const auto move_to = [&](std::size_t pos) {
        st.caret = pos;
        if (!k.select) st.anchor = pos;
    };
    if (k.left)
    {
        // With a selection and no shift, Left collapses to the near EDGE rather than stepping from
        // the caret — otherwise the selection's far end jumps by one and the collapse reads wrong.
        if (st.has_selection() && !k.select) move_to(sel_lo());
        else move_to(k.word ? prev_word(text, st.caret) : prev_codepoint(text, st.caret));
    }
    if (k.right)
    {
        if (st.has_selection() && !k.select) move_to(sel_hi());
        else move_to(k.word ? next_word(text, st.caret) : next_codepoint(text, st.caret));
    }
    if (k.home) move_to(0);
    if (k.end)  move_to(text.size());

    // --- Typed text LAST, so a frame carrying both a caret key and a character applies them in the
    // order they were meant: move, then type where you moved to.
    if (!k.typed.empty())
    {
        erase_selection();
        text.insert(st.caret, k.typed);
        st.caret += k.typed.size();
        st.anchor = st.caret;
        changed = true;
    }

    st.clamp(text);
    return changed;
}

}  // namespace string::ui
