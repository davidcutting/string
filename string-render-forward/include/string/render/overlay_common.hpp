#pragma once

#include <array>
#include <cmath>

#include <string/ui/layout.hpp>

namespace string::render
{

// Authored UI colours are sRGB, but overlay passes draw into the linear HDR offscreen (the
// composite re-encodes to sRGB), so colour channels decode to linear on the way in or they
// brighten. Alpha is already linear and left alone. Shared by the UI and text overlays.
inline float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline std::array<float, 4> to_linear(::string::color c)
{
    return { srgb_to_linear(c.r / 255.0f), srgb_to_linear(c.g / 255.0f),
             srgb_to_linear(c.b / 255.0f), c.a / 255.0f };
}

}  // namespace string::render
