#include "nanoxgen/xgen_color_expression.h"

#include "SeExpression.h"
#include "SeExprFunc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nanoxgen {
namespace {

class PrefVariable final : public SeExprVectorVarRef {
public:
    void set(Vec3 value) noexcept {
        _value.setValue(value.x, value.y, value.z);
    }

    void eval(const SeExprVarNode *, SeVec3d &result) override {
        result = _value;
    }

private:
    SeVec3d _value{};
};

class ColorExpression final : public SeExpression {
public:
    explicit ColorExpression(std::string_view source) {
        static std::once_flag initialize;
        std::call_once(initialize, [] { SeExprFunc::init(); });
        setWantVec(true);
        setExpr(std::string{source});
        if (!isValid()) { throw std::runtime_error(parseError()); }
        if (usesFunc("rand")) {
            throw std::runtime_error(
                "rand() requires XGen expression seed state");
        }
    }

    SeExprVarRef *resolveVar(const std::string &name) const override {
        return name == "Pref" ? &_pref : nullptr;
    }

    Vec3 evaluate(Vec3 reference_position) const {
        _pref.set(reference_position);
        const SeVec3d result = SeExpression::evaluate();
        const Vec3 value{
            static_cast<float>(result[0]),
            static_cast<float>(result[1]),
            static_cast<float>(result[2])};
        if (!std::isfinite(value.x) ||
            !std::isfinite(value.y) ||
            !std::isfinite(value.z)) {
            throw std::runtime_error(
                "expression produced a non-finite color");
        }
        return value;
    }

private:
    mutable PrefVariable _pref;
};

} // namespace

void evaluate_xgen_color_expression(
    std::string_view source,
    std::span<const Vec3> reference_positions,
    std::span<Vec3> values,
    NanoXGenContext *context) {
    if (source.empty()) {
        throw std::invalid_argument("color expression is empty");
    }
    if (values.size() != reference_positions.size()) {
        throw std::invalid_argument(
            "color expression input/output size mismatch");
    }

    constexpr std::size_t minimum_values_per_task = 16384u;
    constexpr std::size_t chunk_size = 4096u;
    const std::size_t useful_tasks = std::max<std::size_t>(
        1u, (values.size() + minimum_values_per_task - 1u) /
                minimum_values_per_task);
    const std::size_t capacity =
        context ? context->worker_count() : available_worker_count();
    const std::size_t task_count = std::min(capacity, useful_tasks);
    const auto evaluate_range = [&](
        ColorExpression &expression, std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            values[index] = expression.evaluate(reference_positions[index]);
        }
    };
    if (task_count <= 1u) {
        ColorExpression expression{source};
        evaluate_range(expression, 0u, values.size());
        return;
    }

    std::unique_ptr<NanoXGenContext> owned_context;
    if (!context) {
        owned_context = std::make_unique<NanoXGenContext>(capacity);
        context = owned_context.get();
    }
    std::atomic_size_t next_value{};
    context->executor().parallel_for(task_count, [&](std::size_t) {
        // SeExpr 1.x keeps local assignment storage in the expression object.
        // One compiled evaluator per task avoids shared mutable state.
        ColorExpression expression{source};
        while (true) {
            const std::size_t begin = next_value.fetch_add(
                chunk_size, std::memory_order_relaxed);
            if (begin >= values.size()) { return; }
            evaluate_range(
                expression, begin,
                std::min(begin + chunk_size, values.size()));
        }
    });
}

} // namespace nanoxgen
