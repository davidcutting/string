#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace string::core
{

// A fixed pool of worker threads draining a shared task queue. Used to move CPU-bound,
// independent work (glTF parsing, texture decode, and later asset streaming / shader watching)
// off the main thread. enqueue() returns a std::future so callers can collect results in order.
//
// Only pure CPU work belongs here — GPU calls (VMA allocation, command recording) are not
// thread-safe against a single pool/allocator and must stay on their owning thread. Exceptions
// thrown by a job are captured in its future and rethrown at .get().
class job_system
{
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;

public:
    // thread_count == 0 uses hardware_concurrency() (at least 1).
    explicit job_system(std::size_t thread_count = 0);
    ~job_system();

    job_system(const job_system&) = delete;
    job_system& operator=(const job_system&) = delete;

    template <typename F>
    auto enqueue(F&& f) -> std::future<std::invoke_result_t<F>>
    {
        using result_t = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<result_t()>>(std::forward<F>(f));
        std::future<result_t> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
            {
                throw std::runtime_error("enqueue on a stopped job_system");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    std::size_t worker_count() const { return workers_.size(); }
};

}  // namespace string::core
