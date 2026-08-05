#include <string/core/cvar.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

namespace string::core
{

// ---------------------------------------------------------------------------
// Name / env mapping helpers
// ---------------------------------------------------------------------------

std::string env_name_for(std::string_view cvar_name)
{
    std::string out;
    out.reserve(cvar_name.size() + 7);
    out = "STRING_";
    for (char c : cvar_name)
    {
        if (c == '.') out.push_back('_');
        else out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool set_env_default(const char* name, const char* value)
{
    if (std::getenv(name) != nullptr) return false;
#if defined(_WIN32)
    // No setenv on Windows. _putenv_s always overwrites, hence the getenv guard above.
    return _putenv_s(name, value) == 0;
#else
    return ::setenv(name, value, /*overwrite=*/0) == 0;
#endif
}

namespace
{
// Trim ASCII whitespace both ends — env values and console tokens can carry stray spaces.
std::string_view trim(std::string_view s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parse_bool(std::string_view v, bool& out)
{
    v = trim(v);
    std::string lower(v);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "1" || lower == "true" || lower == "on" || lower == "yes") { out = true; return true; }
    if (lower == "0" || lower == "false" || lower == "off" || lower == "no") { out = false; return true; }
    return false;
}
}  // namespace

// ---------------------------------------------------------------------------
// Typed parse overrides
// ---------------------------------------------------------------------------

bool CVar<bool>::set_from_string(std::string_view value)
{
    bool parsed{};
    if (!parse_bool(value, parsed)) return false;
    set(parsed);
    return true;
}

bool CVar<int32_t>::set_from_string(std::string_view value)
{
    value = trim(value);
    int32_t parsed{};
    const char* first = value.data();
    const char* last = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) return false;
    set(parsed);
    return true;
}

bool CVar<float>::set_from_string(std::string_view value)
{
    value = trim(value);
    if (value.empty()) return false;
    // std::from_chars<float> support is uneven across libstdc++ versions in this toolchain, so
    // go through strtof on a NUL-terminated copy and require the whole token to be consumed.
    std::string tmp(value);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || end == tmp.c_str()) return false;
    set(parsed);
    return true;
}

// --- string CVar (registry-mutex guarded) ----------------------------------

std::string CVar<std::string>::get() const
{
    std::lock_guard<std::mutex> lock(CVarRegistry::instance().mutex_);
    return value_;
}

void CVar<std::string>::set(std::string_view v)
{
    std::lock_guard<std::mutex> lock(CVarRegistry::instance().mutex_);
    value_.assign(v);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

CVarRegistry& CVarRegistry::instance()
{
    static CVarRegistry registry;
    return registry;
}

void CVarRegistry::register_var(CVarBase* var)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Last-writer-wins on a duplicate primary name: the map simply overwrites. This is a
    // programming error (two CVars claiming one name); the future console will surface it, but we
    // don't throw during static init.
    by_name_[var->name()] = var;
}

void CVarRegistry::unregister_var(CVarBase* var)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = by_name_.find(var->name()); it != by_name_.end() && it->second == var)
        by_name_.erase(it);
    // Drop any aliases pointing at this CVar so a later CVar reusing an address isn't shadowed.
    for (auto it = aliases_.begin(); it != aliases_.end();)
        it = (it->second == var) ? aliases_.erase(it) : std::next(it);
}

void CVarRegistry::register_alias(std::string_view alias, CVarBase* var)
{
    std::lock_guard<std::mutex> lock(mutex_);
    aliases_[std::string(alias)] = var;
}

CVarBase* CVarRegistry::find(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_name_.find(std::string(name));
    return it != by_name_.end() ? it->second : nullptr;
}

CVarBase* CVarRegistry::find_including_aliases(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = by_name_.find(std::string(name)); it != by_name_.end()) return it->second;
    if (auto it = aliases_.find(std::string(name)); it != aliases_.end()) return it->second;
    return nullptr;
}

bool CVarRegistry::set_from_string(std::string_view name, std::string_view value)
{
    CVarBase* var = find_including_aliases(name);
    if (!var) return false;
    return var->set_from_string(value);
}

void CVarRegistry::apply_env()
{
    // Snapshot targets under the lock, then release it — set_from_string on a CVar may re-enter
    // the registry (string CVars take the mutex), so we must not hold it during the apply.
    struct EnvTarget { std::string env; CVarBase* var; };
    std::vector<EnvTarget> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets.reserve(by_name_.size() + aliases_.size());
        for (const auto& [name, var] : by_name_)
            targets.push_back({ env_name_for(name), var });
        // Legacy aliases: the alias string is used verbatim as STRING_<ALIAS> (upper-cased, dots
        // to underscores) so e.g. an alias "hiz" honours STRING_HIZ.
        for (const auto& [alias, var] : aliases_)
            targets.push_back({ env_name_for(alias), var });
    }

    for (const auto& t : targets)
        if (const char* v = std::getenv(t.env.c_str()))
            t.var->set_from_string(v);
}

std::vector<CVarBase*> CVarRegistry::all() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVarBase*> out;
    out.reserve(by_name_.size());
    for (const auto& [name, var] : by_name_) out.push_back(var);
    return out;
}

std::size_t CVarRegistry::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return by_name_.size();
}

}  // namespace string::core
