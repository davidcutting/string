// string_cook — the thin CLI front-end for the asset-bake library (brief 04b).
//
// Cooks source glTFs to `<source>.cooked` blobs next to the source, maintaining a sidecar manifest
// (`<dir>/.cook_manifest`) so re-cooks are incremental (size+mtime fast path, content hash on
// change), and cooks that scene's textures to KTX2/BC7 siblings in the same pass. All the actual
// cook logic lives in string-asset-tools — this is just argv + staleness.
//
// Textures cook here rather than in a separate script so ONE command cooks an asset completely, and
// so each image's transfer function comes from its glTF material role instead of a filename guess
// (see texture_cook.hpp). `--no-textures` cooks geometry only.
//
// Usage:
//   string_cook [--chunk N] [--no-textures] [--force-textures] <file.gltf> [<file2.gltf> ...]
//
// The manifest format + incremental logic land fully in milestone 3; this entry point + the
// straight cook path are wired now so the `.#cook` target builds and the flake-check cook proves it.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <string/core/logger.hpp>

#include <string/asset/tools/bake.hpp>
#include <string/asset/tools/texture_cook.hpp>
#include <string/asset/manifest.hpp>

using namespace string::asset;
using namespace string::asset::tools;

int main(int argc, char** argv)
{
    BakeParams params;
    TextureCookParams tex_params;
    bool cook_textures_too = true;
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
        else if (arg == "--no-textures")
        {
            cook_textures_too = false;
        }
        else if (arg == "--force-textures")
        {
            tex_params.force = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::fprintf(stderr,
                "usage: string_cook [--chunk N | --no-chunk] [--no-textures] [--force-textures]\n"
                "                   <file.gltf> [<file2.gltf> ...]\n");
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
    uint32_t tex_cooked = 0, tex_skipped = 0, tex_failed = 0;
    for (const std::string& in : inputs)
    {
        const std::filesystem::path src = in;
        const std::filesystem::path cooked_path = cooked_path_for(src, params.chunk_max_meshlets);
        try
        {
            CookedScene scene;
            if (!needs_recook(src, cooked_path, params.chunk_max_meshlets))
            {
                STRING_LOG_INFO("[cook] up to date: {}", cooked_path.string());
                ++skipped;
                // Geometry being fresh says nothing about the textures — a new or edited image
                // leaves the .cooked untouched. Read the existing scene back (string-asset's
                // reader, the same one the runtime uses) purely for its texture table, so a
                // texture-only change still cooks.
                if (cook_textures_too && !read_cooked_file(cooked_path, scene))
                    STRING_LOG_WARN("[cook] cannot re-read {} for its texture table",
                                    cooked_path.string());
            }
            else
            {
                STRING_LOG_INFO("[cook] cooking {} (chunk={})", src.string(), params.chunk_max_meshlets);
                scene = bake_gltf(src, params);
                write_cooked_file(scene, cooked_path);
                update_manifest(src, cooked_path, params.chunk_max_meshlets, scene.source_content_hash);
                STRING_LOG_INFO("[cook]   -> {} ({} draws, {} meshlets)", cooked_path.string(),
                                scene.draws.size(), scene.total_meshlets);
                ++cooked;
            }

            if (cook_textures_too && !scene.textures.empty())
            {
                const TextureCookStats ts =
                    cook_scene_textures(scene, src.parent_path(), tex_params);
                STRING_LOG_INFO("[cook]   textures: {} cooked, {} up-to-date, {} failed", ts.cooked,
                                ts.skipped, ts.failed);
                tex_cooked += ts.cooked;
                tex_skipped += ts.skipped;
                tex_failed += ts.failed;
            }
        }
        catch (const std::exception& e)
        {
            STRING_LOG_ERROR("[cook] FAILED {}: {}", src.string(), e.what());
            ++failed;
        }
    }

    STRING_LOG_INFO("[cook] done: scenes {} cooked, {} up-to-date, {} failed; "
                    "textures {} cooked, {} up-to-date, {} failed",
                    cooked, skipped, failed, tex_cooked, tex_skipped, tex_failed);
    // A failed texture is a failed cook: the runtime would silently fall back to stb_image and
    // reintroduce the multi-second decode this whole path exists to remove.
    return (failed || tex_failed) ? 1 : 0;
}
