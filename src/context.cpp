#include "nanoxgen/context.h"

#include <stdexcept>

namespace nanoxgen {

NanoXGenContext::NanoXGenContext()
    : NanoXGenContext{available_worker_count()} {}

NanoXGenContext::NanoXGenContext(std::size_t worker_count)
    : _owned_executor{std::make_unique<ThreadPool>(worker_count)},
      _executor{_owned_executor.get()} {}

NanoXGenContext::NanoXGenContext(TaskExecutor &executor)
    : _executor{&executor} {
    if (executor.worker_count() == 0u) {
        throw std::invalid_argument(
            "NanoXGen context executor reports zero workers");
    }
}

NanoXGenContext::~NanoXGenContext() = default;

TaskExecutor &NanoXGenContext::executor() noexcept {
    return *_executor;
}

const TaskExecutor &NanoXGenContext::executor() const noexcept {
    return *_executor;
}

std::size_t NanoXGenContext::worker_count() const noexcept {
    return _executor->worker_count();
}

bool NanoXGenContext::owns_executor() const noexcept {
    return static_cast<bool>(_owned_executor);
}

} // namespace nanoxgen
