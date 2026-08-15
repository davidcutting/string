#include <string/asset/tools/bake.hpp>

#include <cstring>
#include <fstream>

namespace string::asset::tools
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
    plan[kSecVertices]      = { scene.vertices.size(),          sizeof(string::Vertex), scene.vertices.data() };
    plan[kSecMeshlets]      = { scene.meshlets.size(),          sizeof(GpuMeshlet),     scene.meshlets.data() };
    plan[kSecMeshletVerts]  = { scene.meshlet_vertices.size(),  sizeof(uint32_t),       scene.meshlet_vertices.data() };
    plan[kSecMeshletTris]   = { scene.meshlet_triangles.size(), sizeof(uint32_t),       scene.meshlet_triangles.data() };
    plan[kSecDraws]         = { scene.draws.size(),             sizeof(CookedDraw),     scene.draws.data() };
    plan[kSecMaterials]     = { scene.materials.size(),         sizeof(CookedMaterial), scene.materials.data() };
    plan[kSecTextures]      = { scene.textures.size(),          sizeof(CookedTexture),  scene.textures.data() };
    plan[kSecSkinVerts]     = { scene.skin_vertices.size(),     sizeof(SkinVertex),     scene.skin_vertices.data() };
    plan[kSecSkins]         = { scene.skins.size(),             sizeof(CookedSkin),     scene.skins.data() };
    plan[kSecInverseBind]   = { scene.inverse_bind.size(),      sizeof(glm::mat4),      scene.inverse_bind.data() };
    plan[kSecJointRemap]    = { scene.joint_remap.size(),       sizeof(uint32_t),       scene.joint_remap.data() };
    plan[kSecNames]         = { scene.names.size(),             sizeof(char),           scene.names.data() };

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

namespace
{

// Temp-then-rename so a crash mid-write never leaves a truncated file.
void write_atomic(const std::vector<uint8_t>& blob, const std::filesystem::path& out_path)
{
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
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

}  // namespace

void write_cooked_file(const CookedScene& scene, const std::filesystem::path& out_path)
{
    write_atomic(write_cooked(scene), out_path);
}

void write_anim_pack_file(const std::vector<uint8_t>& pack, const std::filesystem::path& out_path)
{
    write_atomic(pack, out_path);
}

}  // namespace string::asset::tools
