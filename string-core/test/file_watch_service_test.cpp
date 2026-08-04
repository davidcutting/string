#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <string/core/file_watch_service.hpp>
#include <string/core/job_system.hpp>

using namespace string::core;

namespace
{

std::filesystem::path make_temp_file(const std::string& contents)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("fw_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::ofstream(path) << contents;
    return path;
}

// Drive poll until the callback fires or a bounded number of polls elapse (the scan runs on a job
// thread, so a change may take a couple of poll cycles to surface). Returns whether it fired.
bool poll_until(file_watch_service& svc, const bool& flag, int max_polls = 200)
{
    for (int i = 0; i < max_polls && !flag; ++i)
    {
        svc.poll_main_thread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag;
}

}  // namespace

TEST(FileWatchService, FiresOnModification)
{
    job_system jobs(1);
    file_watch_service svc(jobs);

    const auto path = make_temp_file("initial");
    bool fired = false;
    std::filesystem::path fired_path;
    svc.watch(path, [&](const std::filesystem::path& p) { fired = true; fired_path = p; });

    // A registered, unchanged file must not fire.
    svc.poll_main_thread();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    svc.poll_main_thread();
    EXPECT_FALSE(fired);

    // Touch the file with a distinct mtime and expect the callback.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::ofstream(path) << "changed";
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());

    EXPECT_TRUE(poll_until(svc, fired));
    EXPECT_EQ(fired_path, path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(FileWatchService, NoFireWithoutChange)
{
    job_system jobs(1);
    file_watch_service svc(jobs);

    const auto path = make_temp_file("stable");
    bool fired = false;
    svc.watch(path, [&](const std::filesystem::path&) { fired = true; });

    for (int i = 0; i < 20; ++i)
    {
        svc.poll_main_thread();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    EXPECT_FALSE(fired);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
