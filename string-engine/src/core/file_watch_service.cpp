#include <string/core/file_watch_service.hpp>

#include <string/core/logger.hpp>

namespace string::core
{

file_watch_service::file_watch_service(job_system& jobs)
: jobs_(jobs)
{
}

file_watch_service::~file_watch_service()
{
    // Drain any in-flight scan so its captured `this` isn't used after destruction.
    if (scan_in_flight_ && pending_scan_.valid())
    {
        pending_scan_.wait();
    }
}

void file_watch_service::watch(const std::filesystem::path& path, callback cb)
{
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        STRING_LOG_WARN("file_watch_service: cannot stat '{}' — watching anyway", path.string());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({ path, mtime, std::move(cb) });
}

std::vector<std::size_t> file_watch_service::scan_for_changes()
{
    std::vector<std::size_t> changed;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(entries_[i].path, ec);
        if (ec)
        {
            continue;  // file missing mid-edit (e.g. atomic-save rename): try again next poll
        }
        if (mtime != entries_[i].last_write)
        {
            entries_[i].last_write = mtime;
            changed.push_back(i);
        }
    }
    return changed;
}

void file_watch_service::poll_main_thread()
{
    // Reap a completed scan and dispatch its callbacks on this (main) thread.
    if (scan_in_flight_)
    {
        if (pending_scan_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;  // scan still running
        }
        scan_in_flight_ = false;
        const std::vector<std::size_t> changed = pending_scan_.get();
        for (const std::size_t i : changed)
        {
            callback cb;
            std::filesystem::path path;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (i >= entries_.size())
                {
                    continue;
                }
                cb = entries_[i].cb;
                path = entries_[i].path;
            }
            if (cb)
            {
                cb(path);
            }
        }
    }

    // Kick off the next async scan.
    scan_in_flight_ = true;
    pending_scan_ = jobs_.enqueue([this] { return scan_for_changes(); });
}

}  // namespace string::core
