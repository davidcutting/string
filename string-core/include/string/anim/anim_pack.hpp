#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace string::anim
{

// ============================================================================
// The `.anim` pack (brief 23)
// ============================================================================
//
// The sibling file a cooked scene's skins point at: one ozz skeleton blob + one blob per
// animation clip, all opaque ozz::io archive bytes. Same discipline as the cooked-scene
// container: fixed header at offset 0, 16-aligned sections, value-initialized structs so
// padding is zero and a cook is byte-deterministic.
//
// This header is deliberately PURE POD + bounds-checked parsing, with no ozz include: it
// lives in string-core (string-asset cannot be the home — the dependency arrow points from
// string-asset to string-core, and the runtime consumer is string-core's string::anim), and
// only string::anim's .cpp files ever deserialize the blobs through ozz::io::IArchive.
// string-asset-tools writes packs through this same layout (gltf_skin.cpp).
//
// Why a sibling file and not more cooked-scene sections: clips dominate the bytes, a
// paperdoll character is N cooked mesh parts sharing ONE skeleton+clip pack (CookedSkin
// carries skeleton_hash to pair them), and clips iterate independently of geometry. The
// pack is chunk-budget-independent, so the filename carries none: `foo.gltf` -> `foo.anim`
// (see string-asset's anim_path_for()).

inline constexpr char kAnimPackMagic[8] = { 'S', 'T', 'R', 'A', 'N', 'I', 'M', '1' };

// Bump on ANY layout change here — and on any ozz archive-type-version change (Animation=7,
// Skeleton=2 at ozz 0.17.0; static_assert'd where ozz is included), since the blobs' wire
// format is theirs. A version mismatch re-cooks; the runtime never guesses.
inline constexpr uint32_t kAnimPackVersion = 1;

struct AnimClipDesc
{
    char name[64];          // 0   glTF animation name (NUL-terminated, zero-padded)
    uint64_t blob_offset;   // 64  byte offset of the ozz Animation archive, from file start
    uint64_t blob_bytes;    // 72
    float duration;         // 80  seconds (convenience; the blob is authoritative)
    uint32_t track_count;   // 84
    uint32_t _pad[2];       // 88 -> 96
};
static_assert(sizeof(AnimClipDesc) == 96);

struct AnimPackHeader
{
    char magic[8];             // 0   "STRANIM1"
    uint32_t version;          // 8   kAnimPackVersion
    uint32_t joint_count;      // 12  skeleton joint count (convenience)
    uint32_t clip_count;       // 16
    uint32_t _pad0;            // 20
    uint64_t skeleton_hash;    // 24  FNV-1a of the skeleton blob — CookedSkin's pairing check
    uint64_t skeleton_offset;  // 32  byte offset of the ozz Skeleton archive
    uint64_t skeleton_bytes;   // 40
    uint64_t clips_offset;     // 48  AnimClipDesc[clip_count]
};
static_assert(sizeof(AnimPackHeader) == 56);
static_assert(offsetof(AnimPackHeader, skeleton_hash) == 24);

// A parsed pack: owns the file bytes; the accessors are bounds-safe views into them
// (read_anim_pack validated every window before handing the struct back).
struct AnimPack
{
    std::vector<uint8_t> blob;
    AnimPackHeader header{};

    std::span<const uint8_t> skeleton() const;
    std::span<const AnimClipDesc> clips() const;
    std::span<const uint8_t> clip_blob(const AnimClipDesc& clip) const;
};

// Parse a pack from its file bytes. False on bad magic / version / any section window
// falling outside the blob — the caller re-cooks. Never throws on malformed input.
bool read_anim_pack(std::vector<uint8_t> blob, AnimPack& out);

// Load + parse. False if missing/unreadable/malformed.
bool read_anim_pack_file(const std::filesystem::path& path, AnimPack& out);

}  // namespace string::anim
