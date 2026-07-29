#pragma once

#include <string/core/layout.hpp>
#include <string/ui/theme.hpp>

namespace sandbox::ui
{

// The app's palette (brief 12 M0c, L3). The ENGINE owns the theme TYPE (`string::ui::Theme`:
// surfaces, content colours, metrics, motion transitions); the APP owns the VALUES. Brief 05
// shipped these as bare `constexpr` constants, which an element tree can neither inherit from nor
// override per element — both of which the brief-12 conventions require.
//
// One instance, read through `theme()`. The surface/content values are the brief-05 flat/vector
// palette unchanged (sRGB 8-bit; the overlay converts to linear on the way to the HDR target).
[[nodiscard]] const string::ui::Theme& theme();

// Game vocabulary — MEANING, not interaction — stays app-side, by the same line brief 12 draws for
// widgets: item rarity, health/cast fills, hostile/friendly. An engine theme has no business
// knowing what "epic" is.
namespace col
{
inline constexpr string::color hp_fill    { 166, 227, 161, 255 };
inline constexpr string::color hp_bg      { 40, 44, 52, 220 };
inline constexpr string::color cast_fill  { 249, 226, 175, 255 };
inline constexpr string::color hostile    { 243, 139, 168, 255 };
inline constexpr string::color friendly   { 166, 227, 161, 255 };
inline constexpr string::color transparent{ 0, 0, 0, 0 };

// Item rarity colours (WoW-ish), consumed by the inventory screen.
inline constexpr string::color rarity_common   { 200, 200, 200, 255 };
inline constexpr string::color rarity_uncommon { 100, 220, 120, 255 };
inline constexpr string::color rarity_rare     { 100, 160, 245, 255 };
inline constexpr string::color rarity_epic     { 190, 120, 245, 255 };
inline constexpr string::color rarity_legendary{ 245, 180, 90, 255 };
}  // namespace col

}  // namespace sandbox::ui
