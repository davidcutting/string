// string_cook — the thin CLI front-end for the asset-bake library (brief 04b).
//
// Cooks source glTFs to `<source>.cooked` blobs next to the source, maintaining a sidecar manifest
// (`<dir>/.cook_manifest`) so re-cooks are incremental (size+mtime fast path, content hash on
// change). All the actual cook logic lives in assetbake_lib — this is just argv + staleness.
//
// Usage:
//   string_cook [--chunk N] <file.gltf> [<file2.gltf> ...]
//   string_cook [--chunk N] --manifest <dir>          (re-cook every .cooked listed stale in dir)
//
// The manifest format + incremental logic land fully in milestone 3; this entry point + the
// straight cook path are wired now so the `.#cook` target builds and the flake-check cook proves it.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <string/core/logger.hpp>

#include "bake.hpp"
#include "manifest.hpp"

using namespace sandbox::assetbake;

int main(int argc, char** argv)
{
    BakeParams params;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--chunk" && i + 1 < argc)
        {
            params.chunk_max_meshlets = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (arg == "--no-chunk")
        {
            params.chunk_max_meshlets = 0;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::fprintf(stderr,
                "usage: string_cook [--chunk N | --no-chunk] <file.gltf> [<file2.gltf> ...]\n");
            return 0;
        }
        else
        {
            inputs.push_back(arg);
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr, "string_cook: no input glTF given (see --help)\n");
        return 2;
    }

    int cooked = 0, skipped = 0, failed = 0;
    for (const std::string& in : inputs)
    {
        const std::filesystem::path src = in;
        const std::filesystem::path cooked_path = cooked_path_for(src, params.chunk_max_meshlets);
        try
        {
            if (!needs_recook(src, cooked_path, params.chunk_max_meshlets))
            {
                STRING_LOG_INFO("[cook] up to date: {}", cooked_path.string());
                ++skipped;
                continue;
            }
            STRING_LOG_INFO("[cook] cooking {} (chunk={})", src.string(), params.chunk_max_meshlets);
            const CookedScene scene = bake_gltf(src, params);
            write_cooked_file(scene, cooked_path);
            update_manifest(src, cooked_path, params.chunk_max_meshlets, scene.source_content_hash);
            STRING_LOG_INFO("[cook]   -> {} ({} draws, {} meshlets)", cooked_path.string(),
                            scene.draws.size(), scene.total_meshlets);
            ++cooked;
        }
        catch (const std::exception& e)
        {
            STRING_LOG_ERROR("[cook] FAILED {}: {}", src.string(), e.what());
            ++failed;
        }
    }

    STRING_LOG_INFO("[cook] done: {} cooked, {} up-to-date, {} failed", cooked, skipped, failed);
    return failed ? 1 : 0;
}
