#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// CVar system core (brief 06, pulled-forward slice).
//
// Typed console variables declared where they are used via a static registration handle:
//
//     static string::core::CVar<float> lod_error_px{
//         "r.lod.error_px", 0.02f, "screen-space error budget in pixels" };
//     ...
//     float budget = lod_error_px.get();
//
// Every CVar self-registers with a process-global registry on construction, so the future
// console/HUD can enumerate, look up, and show help for every lever without a central list.
//
// Env bridge: at startup CVarRegistry::apply_env() walks the registry and, for each CVar,
// looks for STRING_<NAME> (name upper-cased with '.' -> '_'). If set, it parses and overrides
// the CVar's value. Legacy env names (STRING_HIZ, STRING_CAPTURE_FRAME, ...) are honoured via
// aliases registered alongside the CVar, so existing agent/launch recipes keep working while the
// call sites migrate to the CVar API separately.
//
// Thread-safety: reads are a single relaxed atomic load (bool/int/float) — cheap, lock-free, safe
// from any thread. String CVars are guarded by the registry mutex (rare, not on hot paths). The
// registry's own structure (registration, lookup, env apply) is mutex-guarded and expected to be
// touched at startup / from tooling, not per-frame.
namespace string::core
{

enum class CVarType : uint8_t { Bool, Int, Float, String };

enum class CVarFlags : uint32_t
{
    None    = 0,
    Cheat   = 1u << 0,   // gated behind a cheats-enabled toggle (console honours this later)
    ReadOnly = 1u << 1,  // reported but not settable from the console
};

inline CVarFlags operator|(CVarFlags a, CVarFlags b)
{
    return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_flag(CVarFlags set, CVarFlags f)
{
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(f)) != 0;
}

class CVarBase;
template <typename T> class CVar;

// Process-global registry. All operations are mutex-guarded. The registry never owns the CVars —
// they are typically static-lifetime objects at their declaration sites; the registry just holds
// non-owning pointers and de-registers on CVar destruction.
class CVarRegistry
{
public:
    static CVarRegistry& instance();

    void register_var(CVarBase* var);
    void unregister_var(CVarBase* var);

    // Register an extra name that resolves to `var` (case-insensitive on the STRING_ env bridge).
    // Used to keep legacy STRING_* env names working while call sites migrate.
    void register_alias(std::string_view alias, CVarBase* var);

    // Primary-name lookup (exact). Returns nullptr if unknown.
    CVarBase* find(std::string_view name) const;
    // Lookup honouring aliases as well as primary names.
    CVarBase* find_including_aliases(std::string_view name) const;

    // Set a CVar's value by parsing `value` for its type. Returns false on unknown name or a parse
    // failure (the CVar is left unchanged). Honours aliases.
    bool set_from_string(std::string_view name, std::string_view value);

    // Env bridge. For every registered CVar and alias, probe STRING_<NAME> (dots->underscores,
    // upper-cased) and override the value if present. Idempotent; call once at startup.
    void apply_env();

    // Enumeration for the console/help. Snapshot of the current primary CVars.
    std::vector<CVarBase*> all() const;

    // Number of registered CVars (primary names). For tests/diagnostics.
    std::size_t size() const;

private:
    CVarRegistry() = default;

    // Guards all registry state. CVar value atomics are separate; this only protects the
    // name/alias maps and the string-CVar storage during registration and lookups.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CVarBase*> by_name_;   // primary names
    std::unordered_map<std::string, CVarBase*> aliases_;   // alias -> CVar

    // The registry mutex also guards std::string CVar values (see CVar<std::string>), so those
    // reads/writes route through here. friend keeps that coupling local.
    friend class CVar<std::string>;
};

// Convert a CVar name ("r.lod.error_px") to its env-var form ("STRING_R_LOD_ERROR_PX").
std::string env_name_for(std::string_view cvar_name);

// Set an env lever ONLY if it is not already set, so an explicit value from the user always wins.
// Equivalent to POSIX setenv(name, value, /*overwrite=*/0); Windows has no setenv, only _putenv_s.
// Returns true if this call set it. Not thread-safe against concurrent getenv, like the C calls.
bool set_env_default(const char* name, const char* value);

class CVarBase
{
public:
    CVarBase(std::string_view name, CVarType type, std::string_view help, CVarFlags flags)
    : name_(name), help_(help), type_(type), flags_(flags)
    {
        CVarRegistry::instance().register_var(this);
    }
    virtual ~CVarBase() { CVarRegistry::instance().unregister_var(this); }

    CVarBase(const CVarBase&) = delete;
    CVarBase& operator=(const CVarBase&) = delete;

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    CVarType type() const { return type_; }
    CVarFlags flags() const { return flags_; }

    // Parse `value` for this CVar's type and assign. False = parse failure (value untouched).
    virtual bool set_from_string(std::string_view value) = 0;
    // Current value rendered as text (for help/console listing).
    virtual std::string to_string() const = 0;

    // Register `alias` as an additional name for this CVar (see registry::register_alias).
    void add_alias(std::string_view alias)
    {
        CVarRegistry::instance().register_alias(alias, this);
    }

private:
    std::string name_;
    std::string help_;
    CVarType type_;
    CVarFlags flags_;
};

// Primary template is specialised per supported type below (bool/int32/float/string).

// --- bool -------------------------------------------------------------------
template <>
class CVar<bool> : public CVarBase
{
public:
    CVar(std::string_view name, bool initial, std::string_view help, CVarFlags flags = CVarFlags::None)
    : CVarBase(name, CVarType::Bool, help, flags), value_(initial) {}

    bool get() const { return value_.load(std::memory_order_relaxed); }
    operator bool() const { return get(); }
    void set(bool v) { value_.store(v, std::memory_order_relaxed); }

    bool set_from_string(std::string_view value) override;
    std::string to_string() const override { return get() ? "true" : "false"; }

private:
    std::atomic<bool> value_;
};

// --- int32 ------------------------------------------------------------------
template <>
class CVar<int32_t> : public CVarBase
{
public:
    CVar(std::string_view name, int32_t initial, std::string_view help, CVarFlags flags = CVarFlags::None)
    : CVarBase(name, CVarType::Int, help, flags), value_(initial) {}

    int32_t get() const { return value_.load(std::memory_order_relaxed); }
    operator int32_t() const { return get(); }
    void set(int32_t v) { value_.store(v, std::memory_order_relaxed); }

    bool set_from_string(std::string_view value) override;
    std::string to_string() const override { return std::to_string(get()); }

private:
    std::atomic<int32_t> value_;
};

// --- float ------------------------------------------------------------------
template <>
class CVar<float> : public CVarBase
{
public:
    CVar(std::string_view name, float initial, std::string_view help, CVarFlags flags = CVarFlags::None)
    : CVarBase(name, CVarType::Float, help, flags), value_(initial) {}

    float get() const { return value_.load(std::memory_order_relaxed); }
    operator float() const { return get(); }
    void set(float v) { value_.store(v, std::memory_order_relaxed); }

    bool set_from_string(std::string_view value) override;
    std::string to_string() const override { return std::to_string(get()); }

private:
    std::atomic<float> value_;
};

// --- string -----------------------------------------------------------------
// String values are not lock-free; the registry mutex guards get/set. Reads are rare (console /
// startup), so a mutex is fine and keeps the value coherent under concurrent set.
template <>
class CVar<std::string> : public CVarBase
{
public:
    CVar(std::string_view name, std::string_view initial, std::string_view help,
         CVarFlags flags = CVarFlags::None)
    : CVarBase(name, CVarType::String, help, flags), value_(initial) {}

    std::string get() const;
    void set(std::string_view v);

    bool set_from_string(std::string_view value) override { set(value); return true; }
    std::string to_string() const override { return get(); }

private:
    std::string value_;
};

}  // namespace string::core
