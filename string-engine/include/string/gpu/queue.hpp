#pragma once

#include <cstdint>
#include <optional>

#include <volk.h>

namespace string::gpu
{

enum class queue_type : std::uint8_t
{
    GRAPHICS,
    COMPUTE,
    TRANSFER,
    PRESENT,
};

struct queue_family_indices {
    std::optional<uint32_t> graphics_family;
    std::optional<uint32_t> present_family;
    std::optional<uint32_t> compute_family;
    std::optional<uint32_t> transfer_family;

    bool can_render() { return graphics_family.has_value() && present_family.has_value(); }
    bool has_compute() { return compute_family.has_value(); }
};

struct queue
{
    uint32_t queue_family_index;
    queue_type type;
    VkQueue queue;
};

}