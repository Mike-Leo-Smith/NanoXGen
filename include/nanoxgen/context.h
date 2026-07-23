#pragma once

#include "nanoxgen/task_executor.h"

#include <cstddef>
#include <memory>

namespace nanoxgen {

// CPU execution resources shared across NanoXGen stages. A renderer can
// construct one context around its existing executor; standalone callers can
// let the context own a fixed-size pool. Context does not own GPU devices.
class NanoXGenContext {
public:
    // Create an owned pool sized from the CPUs available to this process.
    NanoXGenContext();
    // Create an owned pool with an explicit, nonzero size.
    explicit NanoXGenContext(std::size_t worker_count);
    // Borrow an external renderer executor. It must outlive this context.
    explicit NanoXGenContext(TaskExecutor &executor);
    ~NanoXGenContext();

    NanoXGenContext(const NanoXGenContext &) = delete;
    NanoXGenContext &operator=(const NanoXGenContext &) = delete;
    NanoXGenContext(NanoXGenContext &&) = delete;
    NanoXGenContext &operator=(NanoXGenContext &&) = delete;

    [[nodiscard]] TaskExecutor &executor() noexcept;
    [[nodiscard]] const TaskExecutor &executor() const noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] bool owns_executor() const noexcept;

private:
    std::unique_ptr<TaskExecutor> _owned_executor;
    TaskExecutor *_executor{};
};

} // namespace nanoxgen
