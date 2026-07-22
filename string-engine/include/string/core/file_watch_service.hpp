#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#include <string/core/job_system.hpp>

namespace string::core
{

// Generic file-watch service: subscribe a path to a callback that fires when the file's
// modification time changes. The locked mechanism is mtime polling (no inotify/Win32 split; SDL3
// has no watcher). Polling runs on the job_system off the main thread; detected changes are
// queued and delivered synchronously from poll_main_thread(), so subscribers' callbacks run on
// the main thread at a frame boundary (safe for GPU work like pipeline recreation).
//
// Shaders are the first consumer; later consumers are VFX data files, tuning/CVars, UI themes.
class file_watch_service
{
public:
    // Fired (on the main thread, from poll_main_thread) when `path`'s mtime advances.
    using callback = std::function<void(const std::filesystem::path& path)>;

    explicit file_watch_service(job_system& jobs);
    ~file_watch_service();

    file_watch_service(const file_watch_service&) = delete;
    file_watch_service& operator=(const file_watch_service&) = delete;

    // Register `path` for watching; `cb` fires whenever the file changes on disk. The initial
    // mtime is recorded now, so a file already on disk does not fire immediately.
    void watch(const std::filesystem::path& path, callback cb);

    // Kick off (or reap) an async mtime scan and deliver any pending change callbacks on the
    // calling thread. Call once per frame from the main loop. Cheap when nothing changed.
    void poll_main_thread();

private:
    struct entry
    {
        std::filesystem::path path;
        std::filesystem::file_time_type last_write;
        callback cb;
    };

    // Scan every entry's mtime; returns the indices whose file changed and updates their record.
    std::vector<std::size_t> scan_for_changes();

    job_system& jobs_;
    std::mutex mutex_;                        // guards entries_ (watch() vs. the scan job)
    std::vector<entry> entries_;
    std::future<std::vector<std::size_t>> pending_scan_;
    bool scan_in_flight_ = false;
};

}  // namespace string::core
