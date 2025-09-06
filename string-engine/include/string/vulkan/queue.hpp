#pragma once

#include <cstdint>
#include <optional>

#include <volk.h>

namespace String
{

enum class QueueType : std::uint8_t
{
    GRAPHICS,
    COMPUTE,
    TRANSFER,
    PRESENT,
};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics_family;
    std::optional<uint32_t> present_family;
    std::optional<uint32_t> compute_family;
    std::optional<uint32_t> transfer_family;

    bool can_render() { return graphics_family.has_value() && present_family.has_value(); }
    bool has_compute() { return compute_family.has_value(); }
};

struct Queue
{
    uint32_t queue_family_index;
    QueueType type;
    VkQueue queue;
};

}