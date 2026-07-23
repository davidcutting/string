#include "bake.hpp"

#include <cstring>
#include <fstream>

#include <string/core/logger.hpp>

namespace sandbox::assetbake
{
namespace
{

// Round `n` up to a 16-byte boundary (section alignment).
uint64_t align16(uint64_t n) { return (n + 15u) & ~uint64_t(15u); }

// One section's element count + element size, for laying out the file.
struct SecPlan { uint64_t count; uint32_t elem; const void* data; };

}  // namespace

std::vector<uint8_t> write_cooked(const CookedScene& scene)
{
    SecPlan plan[kSecCount];
    plan[kSecVertices]      = { scene.vertices.size(),          sizeof(String::Vertex), scene.vertices.data() };
    plan[kSecMeshlets]      = { scene.meshlets.size(),          sizeof(GpuMeshlet),     scene.meshlets.data() };
    plan[kSecMeshletVerts]  = { scene.meshlet_vertices.size(),  sizeof(uint32_t),       scene.meshlet_vertices.data() };
    plan[kSecMeshletTris]   = { scene.meshlet_triangles.size(), sizeof(uint32_t),       scene.meshlet_triangles.data() };
    plan[kSecDraws]         = { scene.draws.size(),             sizeof(CookedDraw),     scene.draws.data() };
    plan[kSecMaterials]     = { scene.materials.size(),         sizeof(CookedMaterial), scene.materials.data() };
    plan[kSecTextures]      = { scene.textures.size(),          sizeof(CookedTexture),  scene.textures.data() };

    // Header value-initialized so its padding bytes are zero (determinism).
    CookedHeader header{};
    std::memcpy(header.magic, kCookedMagic, sizeof(header.magic));
    header.format_version = kCookedFormatVersion;
    header.meshlet_max_vertices = kMeshletMaxVertices;
    header.meshlet_max_triangles = kMeshletMaxTriangles;
    header.max_lods = kMaxLods;
    header.meshlet_cone_weight = kMeshletConeWeight;
    header.chunk_max_meshlets = scene.chunk_max_meshlets;
    header.source_content_hash = scene.source_content_hash;
    header.total_meshlets = scene.total_meshlets;

    // Lay sections out after the header, each 16-aligned.
    uint64_t cursor = align16(sizeof(CookedHeader));
    for (uint32_t s = 0; s < kSecCount; ++s)
    {
        header.sections[s].offset = cursor;
        header.sections[s].count = plan[s].count;
        header.sections[s].elem_size = plan[s].elem;
        cursor += align16(plan[s].count * plan[s].elem);
    }

    std::vector<uint8_t> blob(cursor, 0);   // zero-filled => alignment padding is deterministic
    std::memcpy(blob.data(), &header, sizeof(header));
    for (uint32_t s = 0; s < kSecCount; ++s)
    {
        const uint64_t bytes = plan[s].count * plan[s].elem;
        if (bytes) std::memcpy(blob.data() + header.sections[s].offset, plan[s].data, bytes);
    }
    return blob;
}

void write_cooked_file(const CookedScene& scene, const std::filesystem::path& out_path)
{
    const std::vector<uint8_t> blob = write_cooked(scene);
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    // Temp-then-rename so a crash mid-write never leaves a truncated cooked file.
    const std::filesystem::path tmp = out_path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cook: cannot open " + tmp.string());
        out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!out) throw std::runtime_error("cook: write failed for " + tmp.string());
    }
    std::filesystem::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("cook: rename failed for " + out_path.string());
}

namespace
{

// Bounds-checked view of a section: returns false if the section overruns the blob or the on-disk
// element size doesn't match the compiled struct (layout drift -> re-cook).
template <typename T>
bool load_section(const std::vector<uint8_t>& blob, const CookedSectionDesc& d, std::vector<T>& out)
{
    if (d.elem_size != sizeof(T)) return false;
    const uint64_t bytes = d.count * d.elem_size;
    if (d.offset > blob.size() || bytes > blob.size() - d.offset) return false;
    out.resize(d.count);
    if (bytes) std::memcpy(out.data(), blob.data() + d.offset, bytes);
    return true;
}

}  // namespace

bool read_cooked(const std::vector<uint8_t>& blob, CookedScene& out)
{
    if (blob.size() < sizeof(CookedHeader)) return false;
    CookedHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (std::memcmp(header.magic, kCookedMagic, sizeof(header.magic)) != 0) return false;
    if (header.format_version != kCookedFormatVersion) return false;
    if (header.meshlet_max_vertices != kMeshletMaxVertices ||
        header.meshlet_max_triangles != kMeshletMaxTriangles ||
        header.max_lods != kMaxLods) return false;

    out = CookedScene{};
    out.total_meshlets = header.total_meshlets;
    out.source_content_hash = header.source_content_hash;
    out.chunk_max_meshlets = header.chunk_max_meshlets;

    if (!load_section(blob, header.sections[kSecVertices], out.vertices)) return false;
    if (!load_section(blob, header.sections[kSecMeshlets], out.meshlets)) return false;
    if (!load_section(blob, header.sections[kSecMeshletVerts], out.meshlet_vertices)) return false;
    if (!load_section(blob, header.sections[kSecMeshletTris], out.meshlet_triangles)) return false;
    if (!load_section(blob, header.sections[kSecDraws], out.draws)) return false;
    if (!load_section(blob, header.sections[kSecMaterials], out.materials)) return false;
    if (!load_section(blob, header.sections[kSecTextures], out.textures)) return false;
    return true;
}

bool read_cooked_file(const std::filesystem::path& path, CookedScene& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> blob(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(blob.data()), size)) return false;
    return read_cooked(blob, out);
}

}  // namespace sandbox::assetbake
