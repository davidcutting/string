#include <string/asset/tools/bake.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <meshoptimizer.h>

#include <string/core/logger.hpp>

namespace string::asset::tools
{
namespace
{

// FNV-1a — cheap content hash for staleness/determinism (same scheme as the old meshlet cache).
uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= bytes[i]; h *= 1099511628211ULL; }
    return h;
}

// A geometry sub-job the meshletizer builds meshlets + a LOD chain for: an index list over a
// contiguous local vertex range [vmin, vmax], plus the world AABB the draw's bounds come from. Both
// the whole-draw path (chunking off) and each spatial chunk produce one of these.
struct BakeChunk
{
    std::vector<uint32_t> indices;   // LOD-local? no: still GLOBAL indices; vmin/vmax bound them
    uint32_t vmin = 0;
    uint32_t vmax = 0;
    bool gathered = false;           // split chunk: repack by unique first-use gather (not a range copy)
    glm::vec3 aabb_min{ 0.0f };
    glm::vec3 aabb_max{ 0.0f };
    int32_t material = -1;
    glm::mat4 transform{ 1.0f };
    uint32_t src_index_offset = 0;   // for the streamer-window parity (CookedDraw.index_offset)
    uint32_t src_index_count = 0;
};

// Append the meshlets from one already-built meshopt result (LOD-local vertex/triangle heaps) into
// the scene's GLOBAL heaps, translating meshlet-vertex remap entries into global vertex indices.
// (Copied verbatim from the old meshlet_builder append_lod — determinism-critical: do not reorder.)
struct AppendResult { uint32_t first; uint32_t count; };
AppendResult append_lod(CookedScene& m,
                        const std::vector<meshopt_Meshlet>& mlets,
                        const std::vector<uint32_t>& mverts,
                        const std::vector<unsigned char>& mtris,
                        const std::vector<float>& positions,
                        const std::vector<uint32_t>& local_to_global,
                        size_t used_meshlets)
{
    const uint32_t first = static_cast<uint32_t>(m.meshlets.size());
    for (size_t i = 0; i < used_meshlets; ++i)
    {
        const meshopt_Meshlet& src = mlets[i];
        GpuMeshlet g{};
        g.vertex_offset = static_cast<uint32_t>(m.meshlet_vertices.size());
        g.triangle_offset = static_cast<uint32_t>(m.meshlet_triangles.size());
        g.vertex_count = src.vertex_count;
        g.triangle_count = src.triangle_count;

        for (uint32_t v = 0; v < src.vertex_count; ++v)
        {
            const uint32_t local = mverts[src.vertex_offset + v];
            m.meshlet_vertices.push_back(local_to_global[local]);
        }
        for (uint32_t t = 0; t < src.triangle_count; ++t)
        {
            const unsigned char a = mtris[src.triangle_offset + t * 3 + 0];
            const unsigned char b = mtris[src.triangle_offset + t * 3 + 1];
            const unsigned char c = mtris[src.triangle_offset + t * 3 + 2];
            m.meshlet_triangles.push_back(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16));
        }

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &mverts[src.vertex_offset], &mtris[src.triangle_offset], src.triangle_count,
            positions.data(), positions.size() / 3, sizeof(float) * 3);
        g.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
        g.radius = bounds.radius;
        g.cone_axis = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
        g.cone_cutoff = bounds.cone_cutoff;
        m.meshlets.push_back(g);
    }
    return { first, static_cast<uint32_t>(used_meshlets) };
}

// Meshletize one index list (already remapped to a LOD-local vertex range) and append to the scene.
// (Copied verbatim from the old meshlet_builder — determinism-critical.)
AppendResult meshletize(CookedScene& m, const std::vector<uint32_t>& indices,
                        const std::vector<float>& positions, size_t vertex_count,
                        const std::vector<uint32_t>& local_to_global)
{
    const size_t max_meshlets = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> mlets(max_meshlets);
    std::vector<uint32_t> mverts(max_meshlets * kMeshletMaxVertices);
    std::vector<unsigned char> mtris(max_meshlets * kMeshletMaxTriangles * 3);

    const size_t count = meshopt_buildMeshlets(
        mlets.data(), mverts.data(), mtris.data(), indices.data(), indices.size(),
        positions.data(), vertex_count, sizeof(float) * 3,
        kMeshletMaxVertices, kMeshletMaxTriangles, kMeshletConeWeight);

    for (size_t i = 0; i < count; ++i)
        meshopt_optimizeMeshlet(&mverts[mlets[i].vertex_offset], &mtris[mlets[i].triangle_offset],
                                mlets[i].triangle_count, mlets[i].vertex_count);

    return append_lod(m, mlets, mverts, mtris, positions, local_to_global, count);
}

// Build the LOD0..N meshlet chain for one chunk (index list over [vmin, vmax]) and fill the draw's
// LOD ranges + bounds. `all_positions` is the whole scene's xyz stream (global). This is the exact
// LOD math from meshlet_builder (SSE-driven under-report is intentional — do NOT change it).
void build_lods_for_chunk(CookedScene& scene, const std::vector<float>& all_positions,
                          const BakeChunk& chunk, CookedDraw& draw)
{
    const uint32_t first_meshlet = static_cast<uint32_t>(scene.meshlets.size());
    draw.transform = chunk.transform;
    draw.material = chunk.material;
    draw.aabb_min = chunk.aabb_min;
    draw.aabb_max = chunk.aabb_max;
    draw.center = (chunk.aabb_min + chunk.aabb_max) * 0.5f;
    draw.radius = glm::length(chunk.aabb_max - chunk.aabb_min) * 0.5f;
    draw.vertex_offset = chunk.vmin;
    draw.vertex_count = chunk.indices.empty() ? 0u : (chunk.vmax - chunk.vmin + 1);
    draw.index_offset = chunk.src_index_offset;
    draw.index_count = chunk.src_index_count;

    if (chunk.indices.empty())
    {
        draw.first_meshlet = first_meshlet;
        draw.total_meshlets = 0;
        draw.lod_count = 0;
        return;
    }

    const uint32_t vmin = chunk.vmin;
    const uint32_t vcount = chunk.vmax - vmin + 1;
    std::vector<uint32_t> local_to_global(vcount);
    for (uint32_t v = 0; v < vcount; ++v) local_to_global[v] = vmin + v;
    std::vector<float> local_pos(vcount * 3);
    std::memcpy(local_pos.data(), &all_positions[size_t(vmin) * 3], vcount * 3 * sizeof(float));

    std::vector<uint32_t> lod_indices(chunk.indices.size());
    for (size_t k = 0; k < chunk.indices.size(); ++k) lod_indices[k] = chunk.indices[k] - vmin;

    const float scale = meshopt_simplifyScale(local_pos.data(), vcount, sizeof(float) * 3);

    const std::vector<uint32_t> lod0_indices = lod_indices;
    uint32_t lod_count = 0;
    size_t prev_count = std::numeric_limits<size_t>::max();
    for (uint32_t lod = 0; lod < kMaxLods; ++lod)
    {
        const AppendResult range = meshletize(scene, lod_indices, local_pos, vcount, local_to_global);
        GpuMeshletLod& lod_rec = draw.lods[lod];
        lod_rec.meshlet_offset = range.first;
        lod_rec.meshlet_count = range.count;
        ++lod_count;
        prev_count = lod_indices.size();

        if (lod + 1 >= kMaxLods || lod0_indices.size() <= 128 * 3) break;

        const float frac = 1.0f / float(1u << (lod + 1));
        const size_t target = std::max<size_t>(size_t(float(lod0_indices.size()) * frac) / 3 * 3, 96);
        std::vector<uint32_t> simplified(lod0_indices.size());
        float lod_error = 0.0f;
        const size_t new_count = meshopt_simplify(
            simplified.data(), lod0_indices.data(), lod0_indices.size(),
            local_pos.data(), vcount, sizeof(float) * 3,
            target, /*target_error=*/1.0f, meshopt_SimplifyLockBorder, &lod_error);
        if (new_count == 0 || new_count >= prev_count) break;
        simplified.resize(new_count);
        lod_indices = std::move(simplified);
        draw.lods[lod + 1].error = lod_error * scale;
    }
    draw.lods[0].error = 0.0f;

    const uint32_t total = static_cast<uint32_t>(scene.meshlets.size()) - first_meshlet;
    draw.lod_count = lod_count;
    draw.first_meshlet = first_meshlet;
    draw.total_meshlets = total;
}

// Compute a draw's [vmin, vmax] global vertex range from its index slice.
void index_range_bounds(const std::vector<uint32_t>& indices, uint32_t off, uint32_t count,
                        uint32_t& vmin, uint32_t& vmax)
{
    vmin = std::numeric_limits<uint32_t>::max();
    vmax = 0;
    for (uint32_t k = 0; k < count; ++k)
    {
        const uint32_t v = indices[off + k];
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    if (count == 0) { vmin = 0; vmax = 0; }
}

// World-space AABB of a triangle-index list (positions in glTF/local space, transformed by xf).
void chunk_aabb(const std::vector<uint32_t>& indices, const std::vector<float>& all_positions,
                const glm::mat4& xf, glm::vec3& out_min, glm::vec3& out_max)
{
    out_min = glm::vec3(std::numeric_limits<float>::max());
    out_max = glm::vec3(std::numeric_limits<float>::lowest());
    for (uint32_t idx : indices)
    {
        const glm::vec3 local(all_positions[size_t(idx) * 3 + 0],
                              all_positions[size_t(idx) * 3 + 1],
                              all_positions[size_t(idx) * 3 + 2]);
        const glm::vec3 world = glm::vec3(xf * glm::vec4(local, 1.0f));
        out_min = glm::min(out_min, world);
        out_max = glm::max(out_max, world);
    }
}

// Estimate a chunk's LOD0 meshlet count (upper bound) to decide whether it needs splitting. Uses the
// meshopt bound over the chunk's triangle count — cheap and deterministic (no meshletize needed).
uint32_t estimate_meshlets(size_t index_count)
{
    return static_cast<uint32_t>(
        meshopt_buildMeshletsBound(index_count, kMeshletMaxVertices, kMeshletMaxTriangles));
}

// Recursively median-split a triangle set on the largest-extent axis of its centroid AABB until each
// piece's estimated meshlet count is under `budget`. Deterministic: nth_element with a stable
// tie-break (centroid, then original triangle order) and a fixed recursion order (low half first).
// Emits leaf chunks into `out` in a stable order. `tris` are triangle base-indices into `src`.
void split_recursive(const std::vector<uint32_t>& src, const std::vector<float>& all_positions,
                     std::vector<uint32_t> tris, uint32_t budget, std::vector<std::vector<uint32_t>>& out)
{
    const uint32_t est = estimate_meshlets(tris.size() * 3);
    if (est <= budget || tris.size() <= 1)
    {
        std::vector<uint32_t> leaf;
        leaf.reserve(tris.size() * 3);
        for (uint32_t t : tris)
        {
            leaf.push_back(src[t * 3 + 0]);
            leaf.push_back(src[t * 3 + 1]);
            leaf.push_back(src[t * 3 + 2]);
        }
        out.push_back(std::move(leaf));
        return;
    }

    // Triangle centroid (world/local space consistent — split is transform-invariant, so use local).
    auto centroid = [&](uint32_t t) {
        glm::vec3 c(0.0f);
        for (int j = 0; j < 3; ++j)
        {
            const uint32_t idx = src[t * 3 + j];
            c += glm::vec3(all_positions[size_t(idx) * 3 + 0], all_positions[size_t(idx) * 3 + 1],
                           all_positions[size_t(idx) * 3 + 2]);
        }
        return c / 3.0f;
    };

    glm::vec3 cmin(std::numeric_limits<float>::max()), cmax(std::numeric_limits<float>::lowest());
    for (uint32_t t : tris) { const glm::vec3 c = centroid(t); cmin = glm::min(cmin, c); cmax = glm::max(cmax, c); }
    const glm::vec3 ext = cmax - cmin;
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);

    // Sort by centroid on the chosen axis with a stable tie-break on original triangle index, so the
    // split is fully deterministic regardless of the input's incidental ordering.
    std::sort(tris.begin(), tris.end(), [&](uint32_t a, uint32_t b) {
        const float ca = centroid(a)[axis], cb = centroid(b)[axis];
        if (ca != cb) return ca < cb;
        return a < b;
    });
    const size_t mid = tris.size() / 2;
    std::vector<uint32_t> lo(tris.begin(), tris.begin() + mid);
    std::vector<uint32_t> hi(tris.begin() + mid, tris.end());
    split_recursive(src, all_positions, std::move(lo), budget, out);   // low half first (stable order)
    split_recursive(src, all_positions, std::move(hi), budget, out);
}

}  // namespace

uint64_t content_hash(const std::vector<String::Vertex>& vertices,
                      const std::vector<uint32_t>& indices,
                      const std::vector<GltfDraw>& draws)
{
    // Fold the build parameters + arrays. Draws are hashed field-by-field (GltfDraw has no padding
    // gaps that matter — it's serialized transiently, never on disk — but we hash the raw bytes to
    // match the old cache key discipline and because the caller value-initializes GltfDraw).
    uint64_t h = fnv1a(&kCookedFormatVersion, sizeof(kCookedFormatVersion));
    const uint32_t params[] = { kMeshletMaxVertices, kMeshletMaxTriangles, kMaxLods };
    h = fnv1a(params, sizeof(params), h);
    h = fnv1a(&kMeshletConeWeight, sizeof(kMeshletConeWeight), h);
    h = fnv1a(vertices.data(), vertices.size() * sizeof(String::Vertex), h);
    h = fnv1a(indices.data(), indices.size() * sizeof(uint32_t), h);
    h = fnv1a(draws.data(), draws.size() * sizeof(GltfDraw), h);
    return h;
}

CookedScene bake_scene(const std::vector<String::Vertex>& vertices,
                       const std::vector<uint32_t>& indices,
                       const std::vector<GltfDraw>& draws,
                       const BakeParams& params)
{
    CookedScene scene;
    scene.chunk_max_meshlets = params.chunk_max_meshlets;
    scene.source_content_hash = content_hash(vertices, indices, draws);

    // Extract xyz positions once (meshopt wants a tightly-strided float array).
    std::vector<float> all_positions(vertices.size() * 3);
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        all_positions[i * 3 + 0] = vertices[i].pos.x;
        all_positions[i * 3 + 1] = vertices[i].pos.y;
        all_positions[i * 3 + 2] = vertices[i].pos.z;
    }

    // Phase 1 — build the chunk list (indices stay source-global; splitting reads the source
    // positions). Chunking off (budget 0) => one chunk per draw, byte-identical meshletize to the old
    // per-draw path (the parity gate). Chunking on => oversized draws split spatially, each piece a
    // new draw with tight bounds + its own LOD chain, in a deterministic recursion order.
    std::vector<BakeChunk> chunks;
    for (const GltfDraw& d : draws)
    {
        if (d.index_count == 0)
        {
            BakeChunk empty;
            empty.material = d.material;
            empty.transform = d.transform;
            empty.aabb_min = d.aabb_min;
            empty.aabb_max = d.aabb_max;
            empty.src_index_offset = d.index_offset;
            empty.src_index_count = 0;
            chunks.push_back(std::move(empty));
            continue;
        }

        const uint32_t est = estimate_meshlets(d.index_count);
        const bool split = params.chunk_max_meshlets > 0 && est > params.chunk_max_meshlets;

        if (!split)
        {
            BakeChunk chunk;
            chunk.indices.assign(indices.begin() + d.index_offset,
                                 indices.begin() + d.index_offset + d.index_count);
            index_range_bounds(indices, d.index_offset, d.index_count, chunk.vmin, chunk.vmax);
            chunk.material = d.material;
            chunk.transform = d.transform;
            chunk.aabb_min = d.aabb_min;
            chunk.aabb_max = d.aabb_max;
            chunk.src_index_offset = d.index_offset;
            chunk.src_index_count = d.index_count;
            chunks.push_back(std::move(chunk));
            continue;
        }

        // Spatial split: recursively median-split this draw's triangles into budget-sized pieces.
        std::vector<uint32_t> src(indices.begin() + d.index_offset,
                                  indices.begin() + d.index_offset + d.index_count);
        std::vector<uint32_t> tris(d.index_count / 3);
        for (uint32_t t = 0; t < d.index_count / 3; ++t) tris[t] = t;
        std::vector<std::vector<uint32_t>> leaves;
        split_recursive(src, all_positions, std::move(tris), params.chunk_max_meshlets, leaves);

        for (std::vector<uint32_t>& leaf : leaves)
        {
            BakeChunk chunk;
            chunk.indices = std::move(leaf);
            chunk.gathered = true;
            chunk.material = d.material;
            chunk.transform = d.transform;
            chunk_aabb(chunk.indices, all_positions, d.transform, chunk.aabb_min, chunk.aabb_max);
            chunk.src_index_offset = d.index_offset;
            chunk.src_index_count = static_cast<uint32_t>(chunk.indices.size());
            chunks.push_back(std::move(chunk));
        }
    }

    // Phase 2 — repack the vertex stream grouped per chunk (the format contract: every draw's
    // vertices are one contiguous range, so the streamer suballocates windows whose sum equals the
    // stream size — a split chunk must NOT window its parent's whole [vmin, vmax] span or the heap
    // explodes to the parent-range x chunk-count).
    //   - Unsplit chunks copy their source [vmin, vmax] range VERBATIM (identical local vertex order
    //     => meshopt output is bit-identical to the old per-draw path — the budget-0 parity gate).
    //   - Split chunks gather their unique vertices in first-use order (deterministic).
    // Chunk indices are remapped into the packed stream; [vmin, vmax] becomes the packed range.
    std::vector<String::Vertex> packed;
    packed.reserve(vertices.size());
    for (BakeChunk& c : chunks)
    {
        if (c.indices.empty())
        {
            c.vmin = 0;
            c.vmax = 0;
            continue;
        }
        const uint32_t base = static_cast<uint32_t>(packed.size());
        if (!c.gathered)
        {
            packed.insert(packed.end(), vertices.begin() + c.vmin, vertices.begin() + c.vmax + 1);
            for (uint32_t& i : c.indices) i = (i - c.vmin) + base;
            c.vmax = base + (c.vmax - c.vmin);
            c.vmin = base;
        }
        else
        {
            std::unordered_map<uint32_t, uint32_t> remap;
            remap.reserve(c.indices.size());
            for (uint32_t& i : c.indices)
            {
                const auto [it, inserted] = remap.emplace(i, static_cast<uint32_t>(packed.size()) - base);
                if (inserted) packed.push_back(vertices[i]);
                i = base + it->second;
            }
            c.vmin = base;
            c.vmax = static_cast<uint32_t>(packed.size()) - 1;
        }
    }
    scene.vertices = std::move(packed);

    std::vector<float> packed_positions(scene.vertices.size() * 3);
    for (size_t i = 0; i < scene.vertices.size(); ++i)
    {
        packed_positions[i * 3 + 0] = scene.vertices[i].pos.x;
        packed_positions[i * 3 + 1] = scene.vertices[i].pos.y;
        packed_positions[i * 3 + 2] = scene.vertices[i].pos.z;
    }

    // Phase 3 — meshletize + LOD-chain every chunk against the packed stream.
    for (const BakeChunk& c : chunks)
    {
        scene.draws.emplace_back();
        build_lods_for_chunk(scene, packed_positions, c, scene.draws.back());
    }

    scene.total_meshlets = static_cast<uint32_t>(scene.meshlets.size());
    return scene;
}

}  // namespace string::asset::tools
