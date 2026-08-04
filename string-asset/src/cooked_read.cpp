#include <string/asset/cooked_scene.hpp>

#include <cstring>
#include <fstream>

namespace string::asset
{
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

}  // namespace string::asset
