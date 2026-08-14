#pragma once

#include <cstdint>

namespace string::assets
{

// Stable, opaque asset identity (engine-feature-spec: "stable asset ids + id->path indirection").
// Generational so a stale id held across an unload is detectably invalid rather than silently
// aliasing a later load. Never a path, never an index into a caller-visible vector.
struct asset_id
{
    uint32_t index = ~0u;
    uint32_t generation = 0;
    bool valid() const { return index != ~0u; }
    bool operator==(const asset_id&) const = default;
};

// Registry-GLOBAL handles into the merged content tables. The per-file index spaces of the cooked
// format are rebased exactly once, when a file is inserted into the registry; everything downstream
// speaks these. (Plain indices, not generational: rows are stable for the life of their asset, and
// the asset's generation covers staleness.)
struct mesh_id
{
    uint32_t index = ~0u;
    bool valid() const { return index != ~0u; }
    bool operator==(const mesh_id&) const = default;
};

struct material_id
{
    uint32_t index = ~0u;
    bool valid() const { return index != ~0u; }
    bool operator==(const material_id&) const = default;
};

struct texture_id
{
    uint32_t index = ~0u;
    bool valid() const { return index != ~0u; }
    bool operator==(const texture_id&) const = default;
};

struct skin_id
{
    uint32_t index = ~0u;
    bool valid() const { return index != ~0u; }
    bool operator==(const skin_id&) const = default;
};

}  // namespace string::assets
