#include "nanoxgen/task_executor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace nanoxgen {

std::size_t available_worker_count() noexcept {
#if defined(__linux__)
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) == 0) {
        const int count = CPU_COUNT(&available);
        if (count > 0) { return static_cast<std::size_t>(count); }
    }
#endif
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0u ? 1u : static_cast<std::size_t>(count);
}

struct ThreadPool::Impl {
    struct Group {
        explicit Group(std::size_t count) : remaining{count} {}

        std::atomic_size_t remaining;
        std::mutex mutex;
        std::condition_variable complete;
        std::exception_ptr error;
    };

    explicit Impl(std::size_t count) : count{count} {
        if (count == 0u) {
            throw std::invalid_argument(
                "NanoXGen thread pool needs at least one worker");
        }
        workers.reserve(count);
        try {
            for (std::size_t index = 0u; index < count; ++index) {
                workers.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            {
                std::scoped_lock lock{queue_mutex};
                stopping = true;
            }
            ready.notify_all();
            for (std::thread &worker : workers) {
                if (worker.joinable()) { worker.join(); }
            }
            throw;
        }
    }

    ~Impl() {
        {
            std::scoped_lock lock{queue_mutex};
            stopping = true;
        }
        ready.notify_all();
        for (std::thread &worker : workers) { worker.join(); }
    }

    void worker_loop() {
        active_pool = this;
        while (execute_one(true)) {}
        active_pool = nullptr;
    }

    bool execute_one(bool wait) {
        std::function<void()> job;
        {
            std::unique_lock lock{queue_mutex};
            if (wait) {
                ready.wait(lock, [this] {
                    return stopping || !jobs.empty();
                });
            }
            if (jobs.empty()) { return false; }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
        return true;
    }

    void run(
        std::size_t task_count,
        const std::function<void(std::size_t)> &task) {
        if (task_count == 0u) { return; }
        auto group = std::make_shared<Group>(task_count);
        auto shared_task =
            std::make_shared<std::function<void(std::size_t)>>(task);
        {
            std::scoped_lock lock{queue_mutex};
            if (stopping) {
                throw std::runtime_error(
                    "NanoXGen thread pool is stopping");
            }
            for (std::size_t index = 0u; index < task_count; ++index) {
                jobs.emplace_back([this, group, shared_task, index] {
                    try {
                        (*shared_task)(index);
                    } catch (...) {
                        std::scoped_lock lock{group->mutex};
                        if (!group->error) {
                            group->error = std::current_exception();
                        }
                    }
                    if (group->remaining.fetch_sub(
                            1u, std::memory_order_acq_rel) == 1u) {
                        group->complete.notify_all();
                        // A pool worker waiting inside nested parallel_for()
                        // sleeps on ready so it can also service unrelated
                        // queued work while its own group is incomplete.
                        ready.notify_all();
                    }
                });
            }
        }
        ready.notify_all();

        if (active_pool == this) {
            while (group->remaining.load(std::memory_order_acquire) != 0u) {
                if (execute_one(false)) { continue; }
                std::unique_lock lock{queue_mutex};
                ready.wait(lock, [this, &group] {
                    return stopping || !jobs.empty() ||
                           group->remaining.load(
                               std::memory_order_acquire) == 0u;
                });
            }
        } else {
            std::unique_lock lock{group->mutex};
            group->complete.wait(lock, [&group] {
                return group->remaining.load(
                           std::memory_order_acquire) == 0u;
            });
        }
        std::exception_ptr error;
        {
            std::scoped_lock lock{group->mutex};
            error = group->error;
        }
        if (error) { std::rethrow_exception(error); }
    }

    const std::size_t count;
    std::mutex queue_mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> workers;
    bool stopping{};
    static thread_local Impl *active_pool;
};

thread_local ThreadPool::Impl *ThreadPool::Impl::active_pool = nullptr;

ThreadPool::ThreadPool(std::size_t worker_count)
    : _impl{std::make_unique<Impl>(worker_count)} {}

ThreadPool::~ThreadPool() = default;

std::size_t ThreadPool::worker_count() const noexcept {
    return _impl->count;
}

void ThreadPool::parallel_for(
    std::size_t task_count,
    const std::function<void(std::size_t)> &task) {
    _impl->run(task_count, task);
}

} // namespace nanoxgen
