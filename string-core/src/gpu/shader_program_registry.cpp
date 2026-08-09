#include <string/gpu/shader_program_registry.hpp>

#include <stdexcept>

#include <string/core/logger.hpp>

namespace string::gpu
{

shader_program_registry::shader_program_registry(device& device, shader_compiler& compiler,
                                                 core::file_watch_service& watcher,
                                                 core::job_system& jobs)
: device_(device)
, compiler_(compiler)
, watcher_(watcher)
, jobs_(jobs)
{
}

shader_program* shader_program_registry::create(const std::filesystem::path& source_path,
                                                shader_program::build_fn builder)
{
    auto program = std::make_unique<shader_program>();
    program->source_path_ = source_path;
    program->builder_ = std::move(builder);

    // Up-front compile: a shader broken at boot is a build error, not a hot-reload event.
    compile_error err;
    std::optional<compiled_program> compiled = compiler_.compile(source_path, err);
    if (!compiled)
    {
        throw std::runtime_error("shader_program_registry: failed to compile '" +
                                 source_path.string() + "':\n" + err.message);
    }
    program->active_ = program->builder_(device_, *compiled);

    shader_program* raw = program.get();
    programs_.push_back(std::move(program));

    // Subscribe the source for hot reload.
    watcher_.watch(source_path, [this](const std::filesystem::path& p) { on_file_changed(p); });

    return raw;
}

void shader_program_registry::on_file_changed(const std::filesystem::path& path)
{
    // A single source may back MULTIPLE programs (e.g. one .slang built into two pipeline variants
    // that differ only in fixed state, like a one-sided and a two-sided cull-mode variant). Reload
    // ALL of them, or an edit would silently leave the extra variants running stale SPIR-V.
    std::vector<shader_program*> matches;
    for (const auto& p : programs_)
        if (p->source_path_ == path) matches.push_back(p.get());
    if (matches.empty())
        return;

    STRING_LOG_INFO("shader hot-reload: recompiling '{}'", path.string());

    // Compile once (the compiler's disk cache makes repeat compiles cheap, but a single compile here
    // also guarantees every variant swaps from identical SPIR-V) and stage the result for each variant.
    in_flight_.push_back(jobs_.enqueue([this, matches, path] {
        compile_error err;
        std::optional<compiled_program> compiled = compiler_.compile(path, err);
        std::lock_guard<std::mutex> lock(staged_mutex_);
        for (shader_program* program : matches)
            staged_.push_back({ program, compiled, err });
    }));
}

void shader_program_registry::apply_pending_swaps(const retire_fn& retire)
{
    // Reap finished compile jobs (frees their futures; the results are already staged).
    std::erase_if(in_flight_, [](std::future<void>& f) {
        return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    std::vector<staged_reload> to_apply;
    {
        std::lock_guard<std::mutex> lock(staged_mutex_);
        to_apply.swap(staged_);
    }

    for (staged_reload& s : to_apply)
    {
        if (!s.compiled)
        {
            // Keep last good; record the diagnostic for the overlay.
            s.program->error_ = s.error;
            STRING_LOG_WARN("shader hot-reload FAILED '{}':\n{}",
                            s.program->source_path_.string(), s.error.message);
            continue;
        }

        // Build the new pipeline, then swap at this frame boundary and retire the old one.
        pipeline old = s.program->active_;
        try
        {
            s.program->active_ = s.program->builder_(device_, *s.compiled);
        }
        catch (const std::exception& e)
        {
            s.program->error_ = compile_error{ s.program->source_path_,
                std::string("pipeline build failed: ") + e.what() };
            STRING_LOG_WARN("shader hot-reload build FAILED '{}': {}",
                            s.program->source_path_.string(), e.what());
            continue;
        }
        s.program->error_.reset();  // success clears the overlay for this program
        retire(old);
        STRING_LOG_INFO("shader hot-reload: swapped '{}'", s.program->source_path_.string());
    }
}

std::vector<compile_error> shader_program_registry::current_errors() const
{
    std::vector<compile_error> errors;
    for (const auto& p : programs_)
    {
        if (p->error_)
        {
            errors.push_back(*p->error_);
        }
    }
    return errors;
}

}  // namespace string::gpu
