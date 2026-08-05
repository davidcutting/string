#include <string/gpu/shader_compiler.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <type_traits>
#include <utility>

#ifndef STRING_NO_SLANG
    #include <slang-com-helper.h>
#endif

#include <string/core/logger.hpp>
#include <string/core/profiler.hpp>

namespace string::gpu
{

namespace
{

// FNV-1a over a byte range — cheap, dependency-free content hash for the disk cache key.
uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// The disk cache keys on the top file's content hash, but Slang `import`s (meshlet.slang,
// lighting.slang, sky.slang, ...) are not in that hash — editing an imported module used to leave
// stale cached SPIR-V hits at startup. Fold the whole import closure into the seed by hashing every
// .slang source found in the search dirs (path + content, sorted for determinism). Simple and
// correct: any edit to any importable module invalidates every cached blob. Computed once per
// compiler and reused across compiles (search dirs are fixed for the process lifetime).
uint64_t hash_import_closure(const std::vector<std::filesystem::path>& search_dirs)
{
    // Collect (name, absolute) for every .slang under each search dir. `name` is the path RELATIVE
    // to its search dir, in generic (forward-slash) form.
    //
    // Relative, not absolute, and generic, not native: the key must be LOCATION-INDEPENDENT so a
    // cache baked at package time still hits after the user unzips somewhere else — and
    // platform-independent so a cache baked on Linux hits on Windows. Hashing the absolute native
    // path silently missed every entry when the same files moved directory.
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const auto& dir : search_dirs)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
        {
            continue;
        }
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
             it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".slang")
            {
                std::error_code rel_ec;
                std::filesystem::path rel = std::filesystem::relative(it->path(), dir, rel_ec);
                if (rel_ec || rel.empty())
                {
                    rel = it->path().filename();
                }
                files.emplace_back(rel.generic_string(), it->path());
            }
        }
    }
    // Sort by the relative name so ordering does not depend on install location either.
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    uint64_t h = fnv1a("import-closure", 14);
    for (const auto& [name, path] : files)
    {
        h = fnv1a(name.data(), name.size(), h);
        if (const std::optional<std::string> text = read_text_file(path))
        {
            h = fnv1a(text->data(), text->size(), h);
        }
    }
    return h;
}

#ifndef STRING_NO_SLANG
// Slang's stage enum -> the Vulkan stage bit. Only the stages the engine uses are mapped.
VkShaderStageFlagBits to_vk_stage(SlangStage stage)
{
    switch (stage)
    {
        case SLANG_STAGE_VERTEX:        return VK_SHADER_STAGE_VERTEX_BIT;
        case SLANG_STAGE_FRAGMENT:      return VK_SHADER_STAGE_FRAGMENT_BIT;
        case SLANG_STAGE_COMPUTE:       return VK_SHADER_STAGE_COMPUTE_BIT;
        // HLSL naming: [shader("amplification")] = task, [shader("mesh")] = mesh (brief 03).
        case SLANG_STAGE_AMPLIFICATION: return VK_SHADER_STAGE_TASK_BIT_EXT;
        case SLANG_STAGE_MESH:          return VK_SHADER_STAGE_MESH_BIT_EXT;
        default:                        return VK_SHADER_STAGE_ALL;
    }
}

// Map a Slang binding-type resource shape to the Vulkan descriptor type the bindless table uses.
// The engine's global table is combined-image-sampler (textures) + storage-image + storage-buffer;
// reflection only needs to agree with those so the generated set layout matches the table's.
std::optional<VkDescriptorType> to_vk_descriptor_type(slang::TypeReflection* type)
{
    if (type == nullptr)
    {
        return std::nullopt;
    }
    switch (type->getKind())
    {
        case slang::TypeReflection::Kind::Resource:
        {
            const SlangResourceShape shape = type->getResourceShape();
            const SlangResourceAccess access = type->getResourceAccess();
            const auto base = static_cast<SlangResourceShape>(shape & SLANG_RESOURCE_BASE_SHAPE_MASK);
            if (base == SLANG_STRUCTURED_BUFFER || base == SLANG_BYTE_ADDRESS_BUFFER)
            {
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            if (base == SLANG_TEXTURE_1D || base == SLANG_TEXTURE_2D ||
                base == SLANG_TEXTURE_3D || base == SLANG_TEXTURE_CUBE)
            {
                return access == SLANG_RESOURCE_ACCESS_READ
                    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            return std::nullopt;
        }
        case slang::TypeReflection::Kind::ConstantBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case slang::TypeReflection::Kind::SamplerState:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        default:
            return std::nullopt;
    }
}
#endif  // STRING_NO_SLANG

// --- Program cache (SPIR-V + reflection) -----------------------------------------------------
//
// The cache stores the WHOLE compile result, not just SPIR-V, so a hit needs no Slang at all —
// entry-point names, stages and the reflected layout all come back from disk. That is what lets a
// warm cache skip creating the global session, and is the groundwork for shipping without Slang.
//
// Format is little-endian POD; `pImmutableSamplers` is deliberately not stored because the reflector
// only ever emits nullptr (see the reflection loop below) and a pointer could not round-trip.

constexpr uint32_t kProgramCacheMagic = 0x47525053;  // "SPRG"
constexpr uint32_t kProgramCacheVersion = 1;

template <typename T>
void put(std::string& buf, const T& v)
{
    static_assert(std::is_trivially_copyable_v<T>);
    buf.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

// Reads a T and advances `at`; false if the buffer is too short (truncated/corrupt cache).
template <typename T>
[[nodiscard]] bool get(const std::string& buf, size_t& at, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (at + sizeof(T) > buf.size()) return false;
    std::memcpy(&out, buf.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

void write_program_cache(const std::filesystem::path& file, const compiled_program& program)
{
    std::string buf;
    put(buf, kProgramCacheMagic);
    put(buf, kProgramCacheVersion);

    put(buf, static_cast<uint32_t>(program.stages.size()));
    for (const compiled_stage& s : program.stages)
    {
        put(buf, static_cast<uint32_t>(s.stage));
        put(buf, static_cast<uint32_t>(s.entry_point.size()));
        buf.append(s.entry_point);
        put(buf, static_cast<uint32_t>(s.spirv.size()));
        buf.append(reinterpret_cast<const char*>(s.spirv.data()), s.spirv.size() * sizeof(uint32_t));
    }

    put(buf, static_cast<uint32_t>(program.layout.sets.size()));
    for (const auto& set : program.layout.sets)
    {
        put(buf, static_cast<uint32_t>(set.size()));
        for (const VkDescriptorSetLayoutBinding& b : set)
        {
            put(buf, b.binding);
            put(buf, static_cast<uint32_t>(b.descriptorType));
            put(buf, b.descriptorCount);
            put(buf, static_cast<uint32_t>(b.stageFlags));
        }
    }

    put(buf, static_cast<uint32_t>(program.layout.has_push_constant ? 1 : 0));
    put(buf, static_cast<uint32_t>(program.layout.push_constant.stageFlags));
    put(buf, program.layout.push_constant.offset);
    put(buf, program.layout.push_constant.size);

    // Write-then-rename: a crash mid-write must not leave a truncated file that still parses.
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::optional<compiled_program> read_program_cache(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string buf{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    size_t at = 0;
    uint32_t magic = 0, version = 0;
    if (!get(buf, at, magic) || !get(buf, at, version)) return std::nullopt;
    if (magic != kProgramCacheMagic || version != kProgramCacheVersion) return std::nullopt;

    compiled_program program;

    uint32_t stage_count = 0;
    if (!get(buf, at, stage_count)) return std::nullopt;
    for (uint32_t i = 0; i < stage_count; ++i)
    {
        uint32_t stage = 0, name_len = 0, word_count = 0;
        if (!get(buf, at, stage) || !get(buf, at, name_len)) return std::nullopt;
        if (at + name_len > buf.size()) return std::nullopt;
        std::string name = buf.substr(at, name_len);
        at += name_len;
        if (!get(buf, at, word_count)) return std::nullopt;
        if (at + static_cast<size_t>(word_count) * sizeof(uint32_t) > buf.size()) return std::nullopt;
        std::vector<uint32_t> spirv(word_count);
        std::memcpy(spirv.data(), buf.data() + at, static_cast<size_t>(word_count) * sizeof(uint32_t));
        at += static_cast<size_t>(word_count) * sizeof(uint32_t);
        program.stages.push_back({ static_cast<VkShaderStageFlagBits>(stage), std::move(name),
                                   std::move(spirv) });
    }

    uint32_t set_count = 0;
    if (!get(buf, at, set_count)) return std::nullopt;
    program.layout.sets.resize(set_count);
    for (uint32_t s = 0; s < set_count; ++s)
    {
        uint32_t binding_count = 0;
        if (!get(buf, at, binding_count)) return std::nullopt;
        for (uint32_t b = 0; b < binding_count; ++b)
        {
            uint32_t binding = 0, type = 0, count = 0, stages = 0;
            if (!get(buf, at, binding) || !get(buf, at, type) || !get(buf, at, count) ||
                !get(buf, at, stages))
            {
                return std::nullopt;
            }
            program.layout.sets[s].push_back({
                .binding = binding,
                .descriptorType = static_cast<VkDescriptorType>(type),
                .descriptorCount = count,
                .stageFlags = stages,
                .pImmutableSamplers = nullptr,
            });
        }
    }

    uint32_t has_pc = 0, pc_stages = 0, pc_offset = 0, pc_size = 0;
    if (!get(buf, at, has_pc) || !get(buf, at, pc_stages) || !get(buf, at, pc_offset) ||
        !get(buf, at, pc_size))
    {
        return std::nullopt;
    }
    program.layout.has_push_constant = has_pc != 0;
    program.layout.push_constant = { pc_stages, pc_offset, pc_size };

    if (program.stages.empty()) return std::nullopt;
    return program;
}

}  // namespace

shader_compiler::shader_compiler(std::filesystem::path cache_dir,
                                 std::vector<std::filesystem::path> search_dirs,
                                 std::filesystem::path prebuilt_cache_dir)
: cache_dir_(std::move(cache_dir))
, prebuilt_cache_dir_(std::move(prebuilt_cache_dir))
, search_dirs_(std::move(search_dirs))
{
    // NOTE: the Slang global session is created lazily (ensure_global_session), not here. A run
    // whose shaders all hit the program cache never touches Slang at all.
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    import_closure_hash_ = hash_import_closure(search_dirs_);
}

#ifndef STRING_NO_SLANG
bool shader_compiler::ensure_global_session()
{
    const std::lock_guard lock(global_session_mutex_);
    if (global_session_) return true;
    return !SLANG_FAILED(slang::createGlobalSession(global_session_.writeRef()));
}
#endif

shader_compiler::~shader_compiler() = default;

std::optional<compiled_program> shader_compiler::compile(const std::filesystem::path& source_path,
                                                         compile_error& out_error)
{
    STRING_PROFILE_SCOPE("shader compile")
    const std::optional<std::string> source = read_text_file(source_path);
    if (!source)
    {
        out_error = { source_path, "shader_compiler: could not read source file" };
        return std::nullopt;
    }

    // --- Cache key: hash of the top file's source text, seeded with the schema version AND the
    // import-closure hash (every importable .slang in the search dirs). Folding the closure in means
    // editing an imported module (meshlet.slang, lighting.slang, ...) invalidates the cached result
    // at startup, not just for live-watched top files.
    //
    // Computed BEFORE any Slang call so a hit costs one file read and no compiler at all. Bump the
    // schema when compile options change the emitted SPIR-V or the cache encoding changes.
    constexpr uint64_t kCacheSchemaVersion = 6;
    const uint64_t seed = fnv1a(&import_closure_hash_, sizeof(import_closure_hash_),
                                fnv1a(&kCacheSchemaVersion, sizeof(kCacheSchemaVersion)));
    const uint64_t hash = fnv1a(source->data(), source->size(), seed);
    char key[32];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    const std::string cache_name = std::string(key) + ".program";
    const std::filesystem::path program_cache_file = cache_dir_ / cache_name;

    // Writable cache first, then the read-only one shipped with the package.
    if (std::optional<compiled_program> cached = read_program_cache(program_cache_file))
    {
        return cached;
    }
    if (!prebuilt_cache_dir_.empty())
    {
        if (std::optional<compiled_program> cached =
                read_program_cache(prebuilt_cache_dir_ / cache_name))
        {
            return cached;
        }
    }

#ifdef STRING_NO_SLANG
    // Ship build: no compiler linked. A miss means the package's prebuilt cache does not match these
    // shader sources — almost always a packaging error (cache not regenerated after a shader edit),
    // so say that rather than emitting a bare "compile failed".
    out_error = { source_path,
        "shader_compiler: no cached program for this shader and this build has no compiler "
        "(-Dslang=disabled). The prebuilt cache is missing or stale for key " + std::string(key) +
        " — regenerate it with a slang-enabled build." };
    return std::nullopt;
#else
    if (!ensure_global_session())
    {
        out_error = { source_path, "shader_compiler: failed to create Slang global session" };
        return std::nullopt;
    }

    // --- Session (per-call so compile() is thread-safe on the watch job pool) ---
    slang::TargetDesc target_desc = {};
    target_desc.format = SLANG_SPIRV;
    target_desc.profile = global_session_->findProfile("spirv_1_5");

    std::array<slang::CompilerOptionEntry, 3> options = { {
        { slang::CompilerOptionName::EmitSpirvDirectly,
          { slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr } },
        // Without this Slang renames every entry point to "main" in the SPIR-V,
        // which breaks pipeline creation against the reflected entry-point names.
        { slang::CompilerOptionName::VulkanUseEntryPointName,
          { slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr } },
        // Slang defaults to row-major matrix layout; the C++ side pushes glm (column-major)
        // matrices and ported GLSL semantics assume it. Without this, every mat4 arrives
        // transposed.
        { slang::CompilerOptionName::MatrixLayoutColumn,
          { slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr } },
    } };

    std::vector<const char*> include_paths;
    include_paths.reserve(search_dirs_.size());
    std::vector<std::string> include_storage;
    include_storage.reserve(search_dirs_.size());
    for (const auto& dir : search_dirs_)
    {
        include_storage.push_back(dir.string());
    }
    for (const auto& s : include_storage)
    {
        include_paths.push_back(s.c_str());
    }

    slang::SessionDesc session_desc = {};
    session_desc.targets = &target_desc;
    session_desc.targetCount = 1;
    session_desc.compilerOptionEntries = options.data();
    session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
    session_desc.searchPaths = include_paths.data();
    session_desc.searchPathCount = static_cast<SlangInt>(include_paths.size());

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global_session_->createSession(session_desc, session.writeRef())))
    {
        out_error = { source_path, "shader_compiler: failed to create Slang session" };
        return std::nullopt;
    }

    // --- Load module from source ---
    const std::string module_name = source_path.stem().string();
    Slang::ComPtr<slang::IModule> module;
    {
        Slang::ComPtr<slang::IBlob> diagnostics;
        module = session->loadModuleFromSourceString(
            module_name.c_str(), source_path.string().c_str(), source->c_str(),
            diagnostics.writeRef());
        if (!module)
        {
            out_error = { source_path,
                diagnostics ? static_cast<const char*>(diagnostics->getBufferPointer())
                            : "shader_compiler: unknown module load failure" };
            return std::nullopt;
        }
    }

    // --- Gather every entry point declared in the module ---
    const SlangInt32 entry_point_count = module->getDefinedEntryPointCount();
    if (entry_point_count == 0)
    {
        out_error = { source_path, "shader_compiler: module declares no entry points" };
        return std::nullopt;
    }

    std::vector<slang::IComponentType*> components;
    components.push_back(module);
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entry_points;
    for (SlangInt32 i = 0; i < entry_point_count; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(module->getDefinedEntryPoint(i, ep.writeRef())) || !ep)
        {
            out_error = { source_path, "shader_compiler: failed to fetch entry point" };
            return std::nullopt;
        }
        entry_points.push_back(ep);
        components.push_back(ep);
    }

    // --- Compose + link ---
    Slang::ComPtr<slang::IComponentType> composed;
    {
        Slang::ComPtr<slang::IBlob> diagnostics;
        const SlangResult r = session->createCompositeComponentType(
            components.data(), static_cast<SlangInt>(components.size()),
            composed.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(r))
        {
            out_error = { source_path,
                diagnostics ? static_cast<const char*>(diagnostics->getBufferPointer())
                            : "shader_compiler: composition failed" };
            return std::nullopt;
        }
    }

    Slang::ComPtr<slang::IComponentType> linked;
    {
        Slang::ComPtr<slang::IBlob> diagnostics;
        const SlangResult r = composed->link(linked.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(r))
        {
            out_error = { source_path,
                diagnostics ? static_cast<const char*>(diagnostics->getBufferPointer())
                            : "shader_compiler: link failed" };
            return std::nullopt;
        }
    }

    compiled_program program;
    program.stages.reserve(static_cast<size_t>(entry_point_count));

    for (SlangInt32 i = 0; i < entry_point_count; ++i)
    {
        slang::EntryPointReflection* ep_refl =
            linked->getLayout()->getEntryPointByIndex(i);
        const VkShaderStageFlagBits stage = to_vk_stage(ep_refl->getStage());
        const std::string ep_name = ep_refl->getName();

        Slang::ComPtr<slang::IBlob> spirv_blob;
        Slang::ComPtr<slang::IBlob> diagnostics;
        const SlangResult r = linked->getEntryPointCode(
            i, 0, spirv_blob.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(r) || !spirv_blob)
        {
            out_error = { source_path,
                diagnostics ? static_cast<const char*>(diagnostics->getBufferPointer())
                            : "shader_compiler: SPIR-V generation failed" };
            return std::nullopt;
        }
        const size_t bytes = spirv_blob->getBufferSize();
        std::vector<uint32_t> spirv(bytes / 4);
        std::memcpy(spirv.data(), spirv_blob->getBufferPointer(), bytes);

        program.stages.push_back({ stage, ep_name, std::move(spirv) });
    }

    // --- Reflection: derive descriptor-set bindings + push-constant range from the linked layout ---
    slang::ProgramLayout* prog_layout = linked->getLayout();
    reflected_layout& layout = program.layout;

    // The engine binds one global set (the bindless table). We reflect the global-scope parameters
    // and the entry-point uniform parameters, sort bindings by (set, binding), and collect the
    // push-constant block if present. The bindless arrays report as unbounded resources — we record
    // them at their reported binding with a descriptorCount matching the table.
    const unsigned param_count = prog_layout->getParameterCount();
    for (unsigned p = 0; p < param_count; ++p)
    {
        slang::VariableLayoutReflection* var = prog_layout->getParameterByIndex(p);
        slang::TypeLayoutReflection* type_layout = var->getTypeLayout();
        const slang::ParameterCategory category = var->getCategory();

        if (category == slang::ParameterCategory::PushConstantBuffer)
        {
            // A push-constant block: its element type's uniform size is the range.
            slang::TypeLayoutReflection* element = type_layout->getElementTypeLayout();
            const size_t size = element ? element->getSize() : type_layout->getSize();
            layout.push_constant = {
                .stageFlags = VK_SHADER_STAGE_ALL,
                .offset = 0,
                .size = static_cast<uint32_t>(size),
            };
            layout.has_push_constant = size > 0;
            continue;
        }

        if (category == slang::ParameterCategory::DescriptorTableSlot ||
            category == slang::ParameterCategory::ShaderResource ||
            category == slang::ParameterCategory::UnorderedAccess ||
            category == slang::ParameterCategory::SamplerState ||
            category == slang::ParameterCategory::ConstantBuffer)
        {
            const unsigned set = var->getBindingSpace();
            const unsigned binding = var->getBindingIndex();
            const auto vk_type = to_vk_descriptor_type(type_layout->getType());
            if (!vk_type)
            {
                continue;
            }
            if (layout.sets.size() <= set)
            {
                layout.sets.resize(set + 1);
            }
            layout.sets[set].push_back({
                .binding = binding,
                .descriptorType = *vk_type,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = nullptr,
            });
        }
    }

    // Write-through (best effort; a failed write just costs a recompile next run).
    write_program_cache(program_cache_file, program);
    return program;
#endif  // STRING_NO_SLANG
}

}  // namespace string::gpu
