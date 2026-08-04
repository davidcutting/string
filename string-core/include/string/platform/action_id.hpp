#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Interned input action names.
//
// An action used to be a `std::string` key, hashed and compared on EVERY query — and gameplay queries
// its actions every frame from every system that has an opinion about input. Interning moves that
// work to bind time (and, for a name written as a literal, to COMPILE time): a query is then an
// integer compare.
//
// The name does not disappear. `InputMap` keeps it alongside the binding for the rebinding UI and for
// debug output, which is the only place a human needs to see it.
namespace String
{

// FNV-1a, 64-bit. Same construction as the UI kit's `string::fnv1a` — duplicated rather than shared
// because string-core cannot depend on string-ui, and a hash function is a smaller thing to repeat
// than a dependency edge is to add.
[[nodiscard]] constexpr std::uint64_t fnv1a_64(std::string_view s) noexcept
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : s)
    {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

// A hashed action or context name.
//
// Strongly typed rather than a bare uint64: an action id, a context id and an entity id are all
// 64-bit hashes, and the compiler should be the one noticing when they are mixed up.
struct ActionId
{
    std::uint64_t hash = 0;

    // 0 is the "no action" sentinel — an unnamed action cannot be bound or queried. It is what a
    // default-constructed ActionId is, so a forgotten initialisation fails loudly at the bind rather
    // than silently aliasing some real action.
    [[nodiscard]] constexpr bool valid() const noexcept { return hash != 0; }

    friend constexpr bool operator==(ActionId, ActionId) noexcept = default;
};

[[nodiscard]] constexpr ActionId action_id(std::string_view name) noexcept
{
    return ActionId{ name.empty() ? 0ull : fnv1a_64(name) };
}

namespace literals
{
// `"jump"_action` — consteval, so the hash is a compile-time constant and the query site carries no
// string at all.
[[nodiscard]] consteval ActionId operator""_action(const char* s, std::size_t n)
{
    return action_id(std::string_view{ s, n });
}
}  // namespace literals

}  // namespace String
