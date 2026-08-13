#pragma once

#include <cstdint>
#include <span>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <string/anim/anim.hpp>

// The `.anim` blobs' wire format is ozz's per-type archive version (Animation=7, Skeleton=2
// at ozz 0.17.0). This mirrors the cook-side assert in gltf_skin.cpp: if an ozz upgrade
// bumps either, kAnimPackVersion must bump so every stale pack re-cooks instead of
// deserializing garbage.
static_assert(ozz::io::internal::Version<const ozz::animation::Animation>::kValue == 7,
              "ozz Animation archive version changed: bump kAnimPackVersion and re-cook");
static_assert(ozz::io::internal::Version<const ozz::animation::Skeleton>::kValue == 2,
              "ozz Skeleton archive version changed: bump kAnimPackVersion and re-cook");

namespace string::anim
{

struct Skeleton::Impl
{
    ozz::animation::Skeleton skeleton;
};

struct Clip::Impl
{
    ozz::animation::Animation animation;
};

// Deserialize one ozz object from a pack blob. False on a tag/type mismatch (a stale or
// corrupt pack) — ozz archives carry no checksums, so the tag test is the only cheap guard.
template <typename T>
bool deserialize_ozz(std::span<const uint8_t> blob, T& out)
{
    ozz::io::MemoryStream stream;
    stream.Write(blob.data(), blob.size());
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<T>()) return false;
    archive >> out;
    return true;
}

}  // namespace string::anim
