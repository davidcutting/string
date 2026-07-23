#include "debug_console.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <string/core/cvar.hpp>
#include <string/core/png_writer.hpp>
#include <string/core/stb_image.h>  // declarations only; STB_IMAGE_IMPLEMENTATION lives in geometry_pass.cpp

namespace sandbox
{
using string::core::CVarBase;
using string::core::CVarRegistry;

DebugConsole::DebugConsole()
{
    print(ConsoleLine::Kind::Info, "String debug console. Type 'help' for commands.");
    print(ConsoleLine::Kind::Info, "  <cvar>            print value + help");
    print(ConsoleLine::Kind::Info, "  <cvar> <value>    set value");
    print(ConsoleLine::Kind::Info, "  help [prefix]     list cvars   find <substr>   search");
    print(ConsoleLine::Kind::Info, "  compare a.png b.png   pixel-diff two captures");
}

void DebugConsole::print(ConsoleLine::Kind kind, std::string text)
{
    output_.push_back(ConsoleLine{ kind, std::move(text) });
    while (output_.size() > kMaxOutput) output_.pop_front();
}

void DebugConsole::push_history(const std::string& line)
{
    if (!line.empty() && (history_.empty() || history_.back() != line))
        history_.push_back(line);
    history_cursor_ = -1;
}

std::string DebugConsole::history_prev()
{
    if (history_.empty()) return "";
    if (history_cursor_ == -1) history_cursor_ = int(history_.size()) - 1;
    else if (history_cursor_ > 0) --history_cursor_;
    return history_[history_cursor_];
}

std::string DebugConsole::history_next()
{
    if (history_.empty() || history_cursor_ == -1) return "";
    if (history_cursor_ < int(history_.size()) - 1) { ++history_cursor_; return history_[history_cursor_]; }
    history_cursor_ = -1;
    return "";
}

std::vector<std::string> DebugConsole::complete(const std::string& prefix) const
{
    std::vector<std::string> out;
    for (CVarBase* v : CVarRegistry::instance().all())
        if (v->name().rfind(prefix, 0) == 0) out.push_back(v->name());
    std::sort(out.begin(), out.end());
    return out;
}

void DebugConsole::execute(const std::string& line)
{
    std::string trimmed = line;
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), not_space));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(), trimmed.end());
    if (trimmed.empty()) return;

    print(ConsoleLine::Kind::Echo, "> " + trimmed);
    push_history(trimmed);

    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;

    if (cmd == "help")
    {
        std::string prefix;
        iss >> prefix;
        cmd_help(prefix);
        return;
    }
    if (cmd == "find")
    {
        std::string sub;
        iss >> sub;
        cmd_find(sub);
        return;
    }
    if (cmd == "compare")
    {
        std::string a, b;
        iss >> a >> b;
        cmd_compare(a, b);
        return;
    }
    if (cmd == "clear")
    {
        clear_output();
        return;
    }

    // Otherwise: a cvar name, optionally followed by a value (which may contain spaces for strings).
    std::string value;
    std::getline(iss, value);
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    cmd_cvar(cmd, value, !value.empty());
}

void DebugConsole::cmd_cvar(const std::string& name, const std::string& value, bool has_value)
{
    CVarBase* v = CVarRegistry::instance().find(name);
    if (v == nullptr)
    {
        // Try alias resolution too (legacy names), so "hiz 0" works from the console.
        v = CVarRegistry::instance().find_including_aliases(name);
    }
    if (v == nullptr)
    {
        print(ConsoleLine::Kind::Error, "unknown cvar: " + name);
        // Offer close matches to nudge the user.
        std::vector<std::string> near = complete(name);
        if (!near.empty())
        {
            std::string hint = "did you mean:";
            for (std::size_t i = 0; i < near.size() && i < 6; ++i) hint += " " + near[i];
            print(ConsoleLine::Kind::Info, hint);
        }
        return;
    }
    if (!has_value)
    {
        print(ConsoleLine::Kind::Ok, v->name() + " = " + v->to_string());
        print(ConsoleLine::Kind::Info, "  " + v->help());
        return;
    }
    if (v->set_from_string(value))
        print(ConsoleLine::Kind::Ok, v->name() + " = " + v->to_string());
    else
        print(ConsoleLine::Kind::Error, "could not parse '" + value + "' for " + v->name());
}

void DebugConsole::cmd_help(const std::string& prefix)
{
    std::vector<CVarBase*> all = CVarRegistry::instance().all();
    std::sort(all.begin(), all.end(),
              [](CVarBase* a, CVarBase* b) { return a->name() < b->name(); });
    int shown = 0;
    for (CVarBase* v : all)
    {
        if (!prefix.empty() && v->name().rfind(prefix, 0) != 0) continue;
        print(ConsoleLine::Kind::Info, v->name() + " = " + v->to_string() + "  — " + v->help());
        ++shown;
    }
    if (shown == 0)
        print(ConsoleLine::Kind::Warn, "no cvars match prefix '" + prefix + "'");
}

void DebugConsole::cmd_find(const std::string& substr)
{
    if (substr.empty()) { print(ConsoleLine::Kind::Warn, "usage: find <substr>"); return; }
    int shown = 0;
    for (CVarBase* v : CVarRegistry::instance().all())
    {
        const bool in_name = v->name().find(substr) != std::string::npos;
        const bool in_help = v->help().find(substr) != std::string::npos;
        if (in_name || in_help)
        {
            print(ConsoleLine::Kind::Info, v->name() + " = " + v->to_string() + "  — " + v->help());
            ++shown;
        }
    }
    if (shown == 0) print(ConsoleLine::Kind::Warn, "no cvars match '" + substr + "'");
}

void DebugConsole::cmd_compare(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
    {
        print(ConsoleLine::Kind::Warn, "usage: compare a.png b.png");
        return;
    }
    int wa, ha, ca, wb, hb, cb;
    unsigned char* pa = stbi_load(a.c_str(), &wa, &ha, &ca, 3);
    if (!pa) { print(ConsoleLine::Kind::Error, "cannot read " + a); return; }
    unsigned char* pb = stbi_load(b.c_str(), &wb, &hb, &cb, 3);
    if (!pb) { stbi_image_free(pa); print(ConsoleLine::Kind::Error, "cannot read " + b); return; }

    if (wa != wb || ha != hb)
    {
        print(ConsoleLine::Kind::Error,
              "size mismatch: " + std::to_string(wa) + "x" + std::to_string(ha) + " vs "
              + std::to_string(wb) + "x" + std::to_string(hb));
        stbi_image_free(pa);
        stbi_image_free(pb);
        return;
    }

    const std::size_t n = std::size_t(wa) * ha * 3;
    std::size_t diff_pixels = 0;
    std::uint64_t total_ae = 0;
    std::vector<unsigned char> diff(std::size_t(wa) * ha * 3);
    for (std::size_t p = 0; p < std::size_t(wa) * ha; ++p)
    {
        int d = 0;
        for (int ch = 0; ch < 3; ++ch)
        {
            const int e = std::abs(int(pa[p * 3 + ch]) - int(pb[p * 3 + ch]));
            d = std::max(d, e);
            total_ae += std::uint64_t(e);
            diff[p * 3 + ch] = (unsigned char)std::min(255, e * 4);  // amplify for visibility
        }
        if (d != 0) ++diff_pixels;
    }
    (void)n;

    const std::string diff_path = a.substr(0, a.find_last_of('.')) + "_diff.png";
    const std::vector<std::uint8_t> png = string::core::png::encode(diff.data(), wa, ha, 3);
    std::ofstream out(diff_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    out.close();

    print(diff_pixels == 0 ? ConsoleLine::Kind::Ok : ConsoleLine::Kind::Warn,
          "AE (differing pixels) = " + std::to_string(diff_pixels)
          + " / " + std::to_string(std::size_t(wa) * ha)
          + "  (sum abs = " + std::to_string(total_ae) + ")");
    print(ConsoleLine::Kind::Info, "diff image -> " + diff_path);

    stbi_image_free(pa);
    stbi_image_free(pb);
}

}  // namespace sandbox
