#pragma once

#include <cstdint>

namespace String
{
namespace Layout
{

enum Primitive : std::uint8_t
{
  RECTANGLE,
  ROUNDED_RECTANGLE,
  CIRCLE,
  LINE,
  TRIANGLE
};

struct Vec2
{
  uint16_t x;
  uint16_t y;
};

struct Layout
{
  Vec2 position;
  Vec2 padding;
};

struct Style
{
  uint8_t outline;
  Primitive primitive;
};

}
}
