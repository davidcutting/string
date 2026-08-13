#include <string/anim/anim_pack.hpp>

#include <cstring>
#include <fstream>

namespace string::anim
{
namespace
{

// A section window is valid iff it lies entirely inside the blob (offset first, so the
// subtraction below can't underflow).
bool window_ok(const std::vector<uint8_t>& blob, uint64_t offset, uint64_t bytes)
{
    return offset <= blob.size() && bytes <= blob.size() - offset;
}

}  // namespace

std::span<const uint8_t> AnimPack::skeleton() const
{
    return { blob.data() + header.skeleton_offset, header.skeleton_bytes };
}

std::span<const AnimClipDesc> AnimPack::clips() const
{
    return { reinterpret_cast<const AnimClipDesc*>(blob.data() + header.clips_offset),
             header.clip_count };
}

std::span<const uint8_t> AnimPack::clip_blob(const AnimClipDesc& clip) const
{
    return { blob.data() + clip.blob_offset, clip.blob_bytes };
}

bool read_anim_pack(std::vector<uint8_t> blob, AnimPack& out)
{
    if (blob.size() < sizeof(AnimPackHeader)) return false;
    AnimPackHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (std::memcmp(header.magic, kAnimPackMagic, sizeof(header.magic)) != 0) return false;
    if (header.version != kAnimPackVersion) return false;
    if (!window_ok(blob, header.skeleton_offset, header.skeleton_bytes)) return false;
    if (!window_ok(blob, header.clips_offset,
                   uint64_t(header.clip_count) * sizeof(AnimClipDesc)))
        return false;

    // Validate every clip window (and its name termination) BEFORE exposing accessors, so
    // the accessors can stay unchecked views.
    const auto* descs = reinterpret_cast<const AnimClipDesc*>(blob.data() + header.clips_offset);
    for (uint32_t c = 0; c < header.clip_count; ++c)
    {
        if (!window_ok(blob, descs[c].blob_offset, descs[c].blob_bytes)) return false;
        if (descs[c].name[sizeof(descs[c].name) - 1] != '\0') return false;
    }

    out.blob = std::move(blob);
    out.header = header;
    return true;
}

bool read_anim_pack_file(const std::filesystem::path& path, AnimPack& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> blob(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(blob.data()), size)) return false;
    return read_anim_pack(std::move(blob), out);
}

}  // namespace string::anim
