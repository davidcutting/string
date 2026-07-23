#pragma once

#include <cstdint>
#include <filesystem>

namespace sandbox::assetbake
{

// Cooked-scene staleness + manifest (brief 04b).
//
// A sidecar manifest (`<source-dir>/.cook_manifest`) records, per cooked file: source path, size,
// mtime, chunk budget, and content hash. The CLI re-cooks only stale entries; the engine's
// in-process fallback uses the SAME check to decide whether to cook-on-load.
//
// Fast path: a cooked file is fresh if it exists AND the manifest's recorded (size, mtime, chunk
// budget) match the source's current (size, mtime) and the requested budget. The content hash is a
// slower fallback authority (recomputed only when size/mtime differ) — it's what makes the check
// robust to mtime-only touches and is the determinism/CI anchor.

// The cooked-file path for a source glTF at a given chunk budget. The budget is folded into the name
// so a chunked and an unchunked cook can coexist (and switching the default doesn't silently reuse a
// stale-shaped file): `foo.gltf` -> `foo.c<budget>.cooked` (budget 0 => `foo.c0.cooked`).
std::filesystem::path cooked_path_for(const std::filesystem::path& source, uint32_t chunk_budget);

// True if `source` must be (re)cooked into `cooked_path` at this budget: cooked file missing, or the
// manifest entry missing/mismatched (size, mtime, or budget changed). Does NOT hash unless needed.
// Never throws — returns true (cook) on any I/O uncertainty.
bool needs_recook(const std::filesystem::path& source, const std::filesystem::path& cooked_path,
                  uint32_t chunk_budget);

// Record a successful cook in the manifest (source's current size+mtime, the budget, the content
// hash the bake stamped). Best-effort — logs on failure, never throws.
void update_manifest(const std::filesystem::path& source, const std::filesystem::path& cooked_path,
                     uint32_t chunk_budget, uint64_t content_hash);

}  // namespace sandbox::assetbake
