#pragma once

#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <vector>

// Per-frame string storage for the UI.
//
// `layout_builder` holds text and element names as NON-OWNING views, so every string an author
// produces needs somewhere to live for the rest of the frame. That is all this is — but the shape
// matters, because it is on the hottest path in the kit: a nameplate-heavy frame calls it thousands
// of times.
//
// CHUNKED BUMP ALLOCATION. Appending must never relocate what was already handed out, which is the
// invariant that made the first version a `deque<std::string>` — a vector regrowth moves SSO buffers
// and dangles every view taken so far. A deque satisfies that but pays for it: measured at ~270ns per
// call, dominated by constructing a `std::string`, pushing it onto the deque, and destroying the lot
// at frame start. Blocks give the same non-relocation guarantee with none of that: a copy into the
// current block and a pointer bump, with the blocks REUSED across frames rather than freed.
//
// (`clear()` therefore keeps its memory. That is the point — a UI's per-frame string volume is
// roughly constant, so after the first few frames the allocator is never touched again.)
namespace string::ui
{

class FrameArena
{
public:
    // Copies `s` and returns a view valid until the next `clear()`.
    [[nodiscard]] std::string_view own(std::string_view s)
    {
        if (s.empty()) return {};
        char* dst = allocate(s.size());
        std::memcpy(dst, s.data(), s.size());
        return { dst, s.size() };
    }

    // Format straight into the arena. `arena.format("frame {}", n)` replaces
    // `arena.own("frame " + std::to_string(n))`, which built TWO `std::string` temporaries — one for
    // the number, one for the concatenation — heap-allocated both, copied the result in here, and
    // freed them again, every frame, per label.
    //
    // Formatting into a stack buffer and copying once is deliberately preferred over
    // `formatted_size` + `format_to`, which avoids the copy but walks the format string and the
    // arguments TWICE. UI labels are short; a 256-byte frame is free and one pass beats two. Longer
    // output than that falls back to an exact `std::format`, which allocates — correct, and rare
    // enough not to matter.
    template <typename... Args>
    [[nodiscard]] std::string_view format(std::format_string<Args...> fmt, Args&&... args)
    {
        char buf[256];
        const auto r = std::format_to_n(buf, sizeof buf, fmt, std::forward<Args>(args)...);
        const auto n = static_cast<std::size_t>(r.size);
        if (n <= sizeof buf) return own(std::string_view{ buf, n });
        return own(std::string_view{ std::format(fmt, std::forward<Args>(args)...) });
    }

    // Drops every view handed out, KEEPING the blocks for the next frame.
    void clear() noexcept
    {
        for (Block& b : blocks_) b.used = 0;
        current_ = 0;
    }

    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }
    // Bytes handed out since the last clear — for tests and for anyone chasing a frame that suddenly
    // allocates.
    [[nodiscard]] std::size_t bytes_used() const noexcept
    {
        std::size_t n = 0;
        for (const Block& b : blocks_) n += b.used;
        return n;
    }

private:
    // 64 KiB holds a few thousand short strings, so a typical frame never needs a second block and a
    // heavy one needs a handful. Small enough that an idle UI is not holding megabytes.
    static constexpr std::size_t kBlockSize = 64 * 1024;

    struct Block
    {
        std::unique_ptr<char[]> data;
        std::size_t size = 0;
        std::size_t used = 0;
    };

    char* allocate(std::size_t n)
    {
        // Aligned to a pointer so a future arena user can put something other than chars here; for
        // strings alone it costs a few bytes per allocation and buys not having to revisit this.
        constexpr std::size_t kAlign = alignof(std::max_align_t);
        const std::size_t need = (n + kAlign - 1) & ~(kAlign - 1);

        for (std::size_t i = current_; i < blocks_.size(); ++i)
        {
            if (blocks_[i].used + need <= blocks_[i].size)
            {
                char* p = blocks_[i].data.get() + blocks_[i].used;
                blocks_[i].used += need;
                current_ = i;
                return p;
            }
        }

        // A string bigger than a block gets its own oversized one rather than being truncated or
        // split — rare (a long log line), and silently losing text would be far worse than one
        // outsized allocation.
        const std::size_t size = need > kBlockSize ? need : kBlockSize;
        blocks_.push_back(Block{ std::make_unique<char[]>(size), size, need });
        current_ = blocks_.size() - 1;
        return blocks_.back().data.get();
    }

    std::vector<Block> blocks_;
    std::size_t current_ = 0;
};

}  // namespace string::ui
