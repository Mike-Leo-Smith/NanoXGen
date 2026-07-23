#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace nanoxgen {

// Host-provided executors may adapt an existing renderer job system to this
// interface. parallel_for() must be safe to call concurrently and recursively:
// Classic collection preparation can discover PTEX/clump work inside an
// already-running description task.
class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;

    [[nodiscard]] virtual std::size_t worker_count() const noexcept = 0;
    virtual void parallel_for(
        std::size_t task_count,
        const std::function<void(std::size_t)> &task) = 0;
};

// Reusable fixed-size implementation for standalone tools and applications
// that do not already own a job system. The size is always explicit; NanoXGen
// never guesses it from the build machine or process environment.
class ThreadPool final : public TaskExecutor {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool() override;

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept override;
    void parallel_for(
        std::size_t task_count,
        const std::function<void(std::size_t)> &task) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Logical CPU capacity currently available to this process. Linux affinity
// and cpuset restrictions are honored; other platforms use the standard C++
// hardware-concurrency query. The result is always at least one.
[[nodiscard]] std::size_t available_worker_count() noexcept;

} // namespace nanoxgen
