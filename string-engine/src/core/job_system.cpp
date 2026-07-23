#include <string/core/job_system.hpp>

#include <string>

#include <string/core/profiler.hpp>

namespace string::core
{

job_system::job_system(std::size_t thread_count)
{
    if (thread_count == 0)
    {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0)
        {
            thread_count = 1;
        }
    }

    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i)
    {
        workers_.emplace_back([this, i]() {
            // Name the worker on the Tracy timeline (no-op without -Dtracy). Built once per thread.
            const std::string thread_name = "job worker " + std::to_string(i);
            STRING_PROFILE_THREAD(thread_name.c_str())
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty())
                    {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                {
                    STRING_PROFILE_SCOPE("job execute")
                    task();
                }
            }
        });
    }
}

job_system::~job_system()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

}  // namespace string::core
