#pragma once

#include <deque>
#include <string>
#include <vector>

// Debug console command engine (brief 06). Pure logic — no UI, no GPU — so it is unit-testable and
// the panel author just drives it. Fronts the CVar registry: get/set, tab-completion, help/find, and
// an in-engine PNG compare. Legacy env aliases are untouched (this only reads/writes CVar values).
namespace sandbox
{

// One line of console output, tagged for severity colouring in the panel.
struct ConsoleLine
{
    enum class Kind { Info, Ok, Warn, Error, Echo };
    Kind kind = Kind::Info;
    std::string text;
};

class DebugConsole
{
public:
    DebugConsole();

    // Execute a full command line ("r.lod.error_px 0.05", "help r.", "compare a.png b.png", ...).
    // Appends result lines to the output log (readable via output()).
    void execute(const std::string& line);

    // Tab-completion over CVar names for the current input `prefix`. Returns the matching CVar
    // names (sorted). If exactly one matches, the caller completes to it; if several, they are also
    // echoed to the output as candidates.
    std::vector<std::string> complete(const std::string& prefix) const;

    // History navigation (up/down). Returns the recalled command, or "" past the ends.
    std::string history_prev();
    std::string history_next();
    void push_history(const std::string& line);

    const std::deque<ConsoleLine>& output() const { return output_; }
    void clear_output() { output_.clear(); }

    // Public echo so the panel can surface tab-completion candidates in the log.
    void echo(std::string text) { print(ConsoleLine::Kind::Info, std::move(text)); }

private:
    void print(ConsoleLine::Kind kind, std::string text);
    void cmd_help(const std::string& prefix);
    void cmd_find(const std::string& substr);
    void cmd_compare(const std::string& a, const std::string& b);
    void cmd_cvar(const std::string& name, const std::string& value, bool has_value);

    std::deque<ConsoleLine> output_;
    std::vector<std::string> history_;
    int history_cursor_ = -1;  // -1 = past the newest (blank input line)

    static constexpr std::size_t kMaxOutput = 256;
};

}  // namespace sandbox
