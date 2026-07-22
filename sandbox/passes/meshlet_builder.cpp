#include "meshlet_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>

#include <meshoptimizer.h>

#include <string/core/logger.hpp>

namespace sandbox
{
namespace
{

// FNV-1a — cheap content hash for the disk-cache key (same scheme as the shader cache).
uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= bytes[i]; h *= 1099511628211ULL; }
    return h;
}

// Bump when the meshlet build parameters or the on-disk format change (invalidates cached blobs).
constexpr uint32_t kMeshletCacheVersion = 2;

// Simple length-prefixed vector (de)serialization for the cache blob.
template <typename T>
void write_vec(std::ofstream& out, const std::vector<T>& v)
{
    const uint64_t n = v.size();
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    if (n) out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T)));
}
template <typename T>
bool read_vec(std::ifstream& in, std::vector<T>& v)
{
    uint64_t n = 0;
    if (!in.read(reinterpret_cast<char*>(&n), sizeof(n))) return false;
    v.resize(n);
    if (n && !in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T)))) return false;
    return true;
}

bool load_cache(const std::filesystem::path& file, MeshletModel& m)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    if (!read_vec(in, m.meshlets)) return false;
    if (!read_vec(in, m.meshlet_vertices)) return false;
    if (!read_vec(in, m.meshlet_triangles)) return false;
    if (!read_vec(in, m.draws)) return false;
    if (!read_vec(in, m.draw_ranges)) return false;
    if (!in.read(reinterpret_cast<char*>(&m.total_meshlets), sizeof(m.total_meshlets))) return false;
    return static_cast<bool>(in);
}

void save_cache(const std::filesystem::path& file, const MeshletModel& m)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Temp-then-rename so a crash mid-write never leaves a truncated cache (shader-cache pattern).
    const std::filesystem::path tmp = file.string() + ".tmp";
    { std::ofstream out(tmp, std::ios::binary);
      if (!out) return;
      write_vec(out, m.meshlets);
      write_vec(out, m.meshlet_vertices);
      write_vec(out, m.meshlet_triangles);
      write_vec(out, m.draws);
      write_vec(out, m.draw_ranges);
      out.write(reinterpret_cast<const char*>(&m.total_meshlets), sizeof(m.total_meshlets));
    }
    std::filesystem::rename(tmp, file, ec);
}

// Append the meshlets from one already-built meshopt result (LOD-local vertex/triangle heaps) into
// the model's GLOBAL heaps, translating meshlet-vertex remap entries into global vertex indices.
// Returns the meshlet range [first, count) appended.
struct AppendResult { uint32_t first; uint32_t count; };
AppendResult append_lod(MeshletModel& m,
                        const std::vector<meshopt_Meshlet>& mlets,
                        const std::vector<uint32_t>& mverts,     // LOD-local vertex-index remap
                        const std::vector<unsigned char>& mtris, // LOD-local packed 8-bit tris
                        const std::vector<float>& positions,     // LOD-local vertex positions (xyz)
                        const std::vector<uint32_t>& local_to_global,  // LOD-local vtx -> global vtx
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

        // Copy this meshlet's vertex remap as GLOBAL indices.
        for (uint32_t v = 0; v < src.vertex_count; ++v)
        {
            const uint32_t local = mverts[src.vertex_offset + v];
            m.meshlet_vertices.push_back(local_to_global[local]);
        }
        // Pack the meshlet's 8-bit local triangle indices, one uint per triangle.
        for (uint32_t t = 0; t < src.triangle_count; ++t)
        {
            const unsigned char a = mtris[src.triangle_offset + t * 3 + 0];
            const unsigned char b = mtris[src.triangle_offset + t * 3 + 1];
            const unsigned char c = mtris[src.triangle_offset + t * 3 + 2];
            m.meshlet_triangles.push_back(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16));
        }

        // Bounds + quantized cone from meshopt (operate on the LOD-local heaps + positions).
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

// Meshletize one index list (already remapped to a LOD-local vertex range) and append to the model.
AppendResult meshletize(MeshletModel& m, const std::vector<uint32_t>& indices,
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

}  // namespace

MeshletModel build_meshlets(const std::vector<String::Vertex>& vertices,
                            const std::vector<uint32_t>& indices,
                            const std::vector<GltfDraw>& draws,
                            const std::filesystem::path& cache_dir,
                            uint64_t cache_key_seed)
{
    const auto t0 = std::chrono::steady_clock::now();

    // Cache key: version + seed + hashes of the vertex/index/draw arrays.
    uint64_t hash = fnv1a(&kMeshletCacheVersion, sizeof(kMeshletCacheVersion), fnv1a(&cache_key_seed, sizeof(cache_key_seed)));
    hash = fnv1a(vertices.data(), vertices.size() * sizeof(String::Vertex), hash);
    hash = fnv1a(indices.data(), indices.size() * sizeof(uint32_t), hash);
    hash = fnv1a(draws.data(), draws.size() * sizeof(GltfDraw), hash);
    char key[32];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    const std::filesystem::path cache_file = cache_dir / (std::string("meshlets_") + key + ".bin");

    MeshletModel model;
    if (load_cache(cache_file, model))
    {
        model.from_cache = true;
        model.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return model;
    }

    // Extract xyz positions once (meshopt wants a tightly-strided float array).
    std::vector<float> all_positions(vertices.size() * 3);
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        all_positions[i * 3 + 0] = vertices[i].pos.x;
        all_positions[i * 3 + 1] = vertices[i].pos.y;
        all_positions[i * 3 + 2] = vertices[i].pos.z;
    }

    model.draws.resize(draws.size());
    model.draw_ranges.resize(draws.size());

    for (size_t di = 0; di < draws.size(); ++di)
    {
        const GltfDraw& d = draws[di];
        GpuDrawInfo& info = model.draws[di];
        const uint32_t first_meshlet = static_cast<uint32_t>(model.meshlets.size());

        if (d.index_count == 0)
        {
            model.draw_ranges[di] = { first_meshlet, 0 };
            info.first_meshlet = first_meshlet;
            info.total_meshlets = 0;
            info.lod_count = 0;
            continue;
        }

        // Build a LOD-local vertex range: gather the unique global vertices this draw touches, so the
        // simplifier + meshletizer work on a compact vertex array. meshlet-vertices are translated
        // back to global indices in append_lod (via local_to_global).
        uint32_t vmin = std::numeric_limits<uint32_t>::max();
        uint32_t vmax = 0;
        for (uint32_t k = 0; k < d.index_count; ++k)
        {
            const uint32_t v = indices[d.index_offset + k];
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }
        const uint32_t vcount = vmax - vmin + 1;
        std::vector<uint32_t> local_to_global(vcount);
        for (uint32_t v = 0; v < vcount; ++v) local_to_global[v] = vmin + v;
        std::vector<float> local_pos(vcount * 3);
        std::memcpy(local_pos.data(), &all_positions[size_t(vmin) * 3], vcount * 3 * sizeof(float));

        // LOD0 index list, rebased to the local range.
        std::vector<uint32_t> lod_indices(d.index_count);
        for (uint32_t k = 0; k < d.index_count; ++k) lod_indices[k] = indices[d.index_offset + k] - vmin;

        // meshopt error is normalized; scale by the model extent to get world-space error later.
        const float scale = meshopt_simplifyScale(local_pos.data(), vcount, sizeof(float) * 3);

        // LOD0 always the full mesh (error 0). Each coarser LOD simplifies FROM the original LOD0
        // index list (not the previous LOD) so meshopt's returned result_error is the true deviation
        // relative to LOD0 — accumulating per-step relative errors understates the total badly (each
        // step's error is relative to the shrinking previous mesh), which drove the parity failure:
        // coarse LODs were selected far too close because their reported error looked tiny.
        const std::vector<uint32_t> lod0_indices = lod_indices;
        uint32_t lod_count = 0;
        size_t prev_count = std::numeric_limits<size_t>::max();
        for (uint32_t lod = 0; lod < kMaxLods; ++lod)
        {
            const AppendResult range = meshletize(model, lod_indices, local_pos, vcount, local_to_global);
            GpuMeshletLod& lod_rec = info.lods[lod];
            lod_rec.meshlet_offset = range.first;
            lod_rec.meshlet_count = range.count;
            ++lod_count;
            prev_count = lod_indices.size();

            // Stop once down to a handful of triangles or the last LOD.
            if (lod + 1 >= kMaxLods || lod0_indices.size() <= 128 * 3) break;

            // Next LOD: target a fraction of the ORIGINAL index count (halving per level), simplifying
            // LOD0 each time. result_error is relative to LOD0's extent -> multiply by scale for world
            // units. LockBorder keeps shared edges welded so adjacent draws don't crack apart.
            const float frac = 1.0f / float(1u << (lod + 1));   // 1/2, 1/4, 1/8, ...
            const size_t target = std::max<size_t>(size_t(float(lod0_indices.size()) * frac) / 3 * 3, 96);
            std::vector<uint32_t> simplified(lod0_indices.size());
            float lod_error = 0.0f;
            const size_t new_count = meshopt_simplify(
                simplified.data(), lod0_indices.data(), lod0_indices.size(),
                local_pos.data(), vcount, sizeof(float) * 3,
                target, /*target_error=*/1.0f, meshopt_SimplifyLockBorder, &lod_error);
            // No progress vs the previous LOD -> stop the chain (avoid duplicate LODs).
            if (new_count == 0 || new_count >= prev_count) break;
            simplified.resize(new_count);
            lod_indices = std::move(simplified);
            // Error stored on the NEXT record is this simplification's deviation from LOD0.
            info.lods[lod + 1].error = lod_error * scale;
        }
        info.lods[0].error = 0.0f;

        const uint32_t total = static_cast<uint32_t>(model.meshlets.size()) - first_meshlet;
        info.lod_count = lod_count;
        info.first_meshlet = first_meshlet;
        info.total_meshlets = total;
        info.center = (d.aabb_min + d.aabb_max) * 0.5f;
        info.radius = glm::length(d.aabb_max - d.aabb_min) * 0.5f;
        model.draw_ranges[di] = { first_meshlet, total };
    }

    model.total_meshlets = static_cast<uint32_t>(model.meshlets.size());

    // Debug validation (STRING_MESHLET_VALIDATE=1): reconstruct each draw's LOD0 triangles from the
    // meshlet data and compare against the original index list (canonicalized multiset).
    if (const char* env = std::getenv("STRING_MESHLET_VALIDATE"); env && env[0] == '1')
    {
        int bad_reported = 0;
        for (size_t di = 0; di < draws.size(); ++di)
        {
            const GltfDraw& d = draws[di];
            const GpuDrawInfo& info = model.draws[di];
            if (d.index_count == 0 || info.lod_count == 0) continue;

            auto canon = [](uint32_t a, uint32_t b, uint32_t c) {
                if (a > b) std::swap(a, b);
                if (b > c) std::swap(b, c);
                if (a > b) std::swap(a, b);
                return (uint64_t(a) << 42) ^ (uint64_t(b) << 21) ^ uint64_t(c);
            };
            std::vector<uint64_t> orig, rec;
            orig.reserve(d.index_count / 3);
            for (uint32_t k = 0; k + 2 < d.index_count; k += 3)
                orig.push_back(canon(indices[d.index_offset + k], indices[d.index_offset + k + 1],
                                     indices[d.index_offset + k + 2]));

            const GpuMeshletLod& l0 = info.lods[0];
            uint32_t oob_verts = 0;
            for (uint32_t m = 0; m < l0.meshlet_count; ++m)
            {
                const GpuMeshlet& ml = model.meshlets[l0.meshlet_offset + m];
                for (uint32_t t = 0; t < ml.triangle_count; ++t)
                {
                    const uint32_t packed = model.meshlet_triangles[ml.triangle_offset + t];
                    const uint32_t a = model.meshlet_vertices[ml.vertex_offset + (packed & 0xFF)];
                    const uint32_t b = model.meshlet_vertices[ml.vertex_offset + ((packed >> 8) & 0xFF)];
                    const uint32_t c = model.meshlet_vertices[ml.vertex_offset + ((packed >> 16) & 0xFF)];
                    rec.push_back(canon(a, b, c));
                    oob_verts += (packed & 0xFF) >= ml.vertex_count
                               || ((packed >> 8) & 0xFF) >= ml.vertex_count
                               || ((packed >> 16) & 0xFF) >= ml.vertex_count;
                }
            }
            std::sort(orig.begin(), orig.end());
            std::sort(rec.begin(), rec.end());
            if (orig != rec || oob_verts)
            {
                if (bad_reported++ < 10)
                    STRING_LOG_WARN("[validate] draw {} LOD0 MISMATCH: orig {} tris, rebuilt {} tris, oob {}",
                                    di, orig.size(), rec.size(), oob_verts);
            }
        }
        STRING_LOG_INFO("[validate] LOD0 triangle-set check complete");
    }

    save_cache(cache_file, model);
    model.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return model;
}

}  // namespace sandbox
