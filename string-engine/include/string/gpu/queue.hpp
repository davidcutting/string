#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Brief 04e M1: what one queue family of the selected physical device actually offers. The device
// enumerates this table once at selection; queue/lane creation and the render graph's placement
// policy read it instead of re-probing (and instead of assuming hardware shape).
struct queue_family_caps
{
    uint32_t index = 0;
    VkQueueFlags flags = 0;       // graphics/compute/transfer/sparse bits as reported
    uint32_t queue_count = 0;     // queues the family exposes
    bool present_support = false;
    bool timestamps = false;      // timestampValidBits > 0 (GPU profiling zones are meaningful)
};

// Brief 04e M1: a named submission lane — the engine-facing LOGICAL handle onto one REAL queue,
// chosen once at device init. Guardrail (locked): a lane maps 1:1 to a hardware queue; if the
// hardware exposes fewer queues, fewer lanes EXIST — there is no transparent multiplexing.
// Placement/pooling decisions live in the graph scheduler, never behind individual submits.
// Names: "main" (graphics, always present), "async-compute-N" (dedicated compute family, one per
// created queue), "transfer" (dedicated transfer family, if any).
struct submission_lane
{
    std::string name;
    queue_type type = queue_type::GRAPHICS;
    uint32_t queue_family_index = 0;
    uint32_t queue_index = 0;     // index within the family
    VkQueue vk_queue = VK_NULL_HANDLE;
    bool timestamps = false;

    // The legacy queue handle shape (command_recorder::init and friends take it).
    struct queue as_queue() const;
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

inline queue submission_lane::as_queue() const
{
    return { queue_family_index, type, vk_queue };
}

}