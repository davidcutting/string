#include <string/asset/manifest.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <string/anim/anim_pack.hpp>
#include <string/core/logger.hpp>

#include <string/asset/cooked_format.hpp>

namespace string::asset
{
namespace
{

// One manifest entry, keyed by the cooked file's name (unique within a source dir).
struct Entry
{
    uint64_t source_size = 0;
    int64_t source_mtime = 0;   // ns since epoch (file_time -> a stable integer)
    uint32_t chunk_budget = 0;
    uint64_t content_hash = 0;
};

int64_t mtime_ns(const std::filesystem::path& p, std::error_code& ec)
{
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

std::filesystem::path manifest_path(const std::filesystem::path& source)
{
    return source.parent_path() / ".cook_manifest";
}

// Load the manifest into a name->Entry map. Missing/unreadable => empty map (everything looks stale).
std::map<std::string, Entry> load_manifest(const std::filesystem::path& mpath)
{
    std::map<std::string, Entry> out;
    std::ifstream in(mpath);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        Entry e;
        // Line format: <cooked-name>\t<size>\t<mtime_ns>\t<budget>\t<hash>
        if (ss >> name >> e.source_size >> e.source_mtime >> e.chunk_budget >> e.content_hash)
            out[name] = e;
    }
    return out;
}

void save_manifest(const std::filesystem::path& mpath, const std::map<std::string, Entry>& entries)
{
    std::error_code ec;
    std::filesystem::create_directories(mpath.parent_path(), ec);
    const std::filesystem::path tmp = mpath.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { STRING_LOG_WARN("[cook] cannot write manifest {}", tmp.string()); return; }
        out << "# string cook manifest: <cooked-name>\\t<size>\\t<mtime_ns>\\t<budget>\\t<hash>\n";
        for (const auto& [name, e] : entries)
            out << name << '\t' << e.source_size << '\t' << e.source_mtime << '\t'
                << e.chunk_budget << '\t' << e.content_hash << '\n';
    }
    std::filesystem::rename(tmp, mpath, ec);
    if (ec) STRING_LOG_WARN("[cook] manifest rename failed {}", mpath.string());
}

}  // namespace

std::filesystem::path cooked_path_for(const std::filesystem::path& source, uint32_t chunk_budget)
{
    std::filesystem::path out = source;
    out.replace_extension(".c" + std::to_string(chunk_budget) + ".cooked");
    return out;
}

std::filesystem::path anim_path_for(const std::filesystem::path& source)
{
    std::filesystem::path out = source;
    out.replace_extension(".anim");
    return out;
}

bool needs_recook(const std::filesystem::path& source, const std::filesystem::path& cooked_path,
                  uint32_t chunk_budget)
{
    std::error_code ec;
    if (!std::filesystem::exists(cooked_path, ec) || ec) return true;

    // Format-version fast check (the size/mtime manifest can't see a code-side format bump — the
    // SOURCE didn't change): peek the cooked header's magic + version; mismatch => recook. The
    // full header is read because the skins section count decides the `.anim` check below.
    {
        std::ifstream in(cooked_path, std::ios::binary);
        CookedHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.magic, kCookedMagic, sizeof(header.magic)) != 0 ||
            header.format_version != kCookedFormatVersion)
            return true;

        // Brief 23: a skinned scene's `.anim` sibling must exist with a current magic + version
        // (a deleted pack, or a pack-format bump with an unchanged source, re-cooks). Static
        // scenes (no skins) skip this entirely — no perpetual re-cook for sponza.
        if (header.sections[kSecSkins].count > 0)
        {
            std::ifstream anim_in(anim_path_for(source), std::ios::binary);
            char anim_magic[8] = {};
            uint32_t anim_version = 0;
            anim_in.read(anim_magic, sizeof(anim_magic));
            anim_in.read(reinterpret_cast<char*>(&anim_version), sizeof(anim_version));
            if (!anim_in ||
                std::memcmp(anim_magic, anim::kAnimPackMagic, sizeof(anim_magic)) != 0 ||
                anim_version != anim::kAnimPackVersion)
                return true;
        }
    }

    const std::map<std::string, Entry> entries = load_manifest(manifest_path(source));
    const std::string key = cooked_path.filename().string();
    const auto it = entries.find(key);
    if (it == entries.end()) return true;

    const uint64_t size = std::filesystem::file_size(source, ec);
    if (ec) return true;
    const int64_t mtime = mtime_ns(source, ec);
    if (ec) return true;

    const Entry& e = it->second;
    // Fast path: size + mtime + budget all match -> fresh. (The content hash is the deeper
    // authority; it's stamped in the cooked header and would only be re-checked on a size/mtime
    // change, at which point we simply re-cook — recomputing it here would need a full flatten.)
    return !(e.source_size == size && e.source_mtime == mtime && e.chunk_budget == chunk_budget);
}

void update_manifest(const std::filesystem::path& source, const std::filesystem::path& cooked_path,
                     uint32_t chunk_budget, uint64_t content_hash)
{
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(source, ec);
    const int64_t mtime = mtime_ns(source, ec);
    if (ec)
    {
        STRING_LOG_WARN("[cook] cannot stat {} for manifest", source.string());
        return;
    }
    const std::filesystem::path mpath = manifest_path(source);
    std::map<std::string, Entry> entries = load_manifest(mpath);
    entries[cooked_path.filename().string()] = Entry{ size, mtime, chunk_budget, content_hash };
    save_manifest(mpath, entries);
}

}  // namespace string::asset
