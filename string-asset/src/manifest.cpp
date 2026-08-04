#include <string/asset/manifest.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

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

bool needs_recook(const std::filesystem::path& source, const std::filesystem::path& cooked_path,
                  uint32_t chunk_budget)
{
    std::error_code ec;
    if (!std::filesystem::exists(cooked_path, ec) || ec) return true;

    // Format-version fast check (the size/mtime manifest can't see a code-side format bump — the
    // SOURCE didn't change): peek the cooked header's magic + version; mismatch => recook.
    {
        std::ifstream in(cooked_path, std::ios::binary);
        char magic[8] = {};
        uint32_t version = 0;
        in.read(magic, sizeof(magic));
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (!in || std::memcmp(magic, kCookedMagic, sizeof(magic)) != 0 ||
            version != kCookedFormatVersion)
            return true;
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
