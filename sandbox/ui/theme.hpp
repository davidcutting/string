#pragma once

#include <string/core/layout.hpp>

namespace sandbox::ui
{

// Flat/vector visual language palette (brief 05). sRGB 8-bit; the overlay converts to linear on the
// way to the HDR target. Centralised so every screen/widget reads the same theme.
namespace col
{
inline constexpr string::color panel      { 26, 28, 42, 235 };
inline constexpr string::color panel_alt  { 20, 22, 34, 235 };
inline constexpr string::color stroke     { 70, 74, 100, 255 };
inline constexpr string::color stroke_hi  { 137, 180, 250, 255 };
inline constexpr string::color text       { 205, 214, 244, 255 };
inline constexpr string::color text_dim   { 140, 147, 175, 255 };
inline constexpr string::color accent     { 137, 180, 250, 255 };  // blue
inline constexpr string::color accent_warm{ 249, 226, 175, 255 };  // yellow
inline constexpr string::color good       { 166, 227, 161, 255 };  // green
inline constexpr string::color bad        { 243, 139, 168, 255 };  // red/pink
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
