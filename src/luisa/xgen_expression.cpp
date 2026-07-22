#include "nanoxgen/luisa/xgen_expression.h"

namespace nanoxgen::luisa_backend {

using namespace luisa;
using namespace luisa::compute;

namespace {

Expr<float> lower_ramp(span<const XgenFloatRampPoint> points,
                       Expr<float> parameter) noexcept {
    Float result = clamp(points.front().value, 0.0f, 1.0f);
    for (std::size_t upper = 1u; upper < points.size(); ++upper) {
        const std::size_t lower = upper - 1u;
        const float p0 = points[lower].position;
        const float p1 = points[upper].position;
        const float k0 = points[lower].value;
        const float k1 = points[upper].value;
        Float u = (parameter - p0) / (p1 - p0);
        Float candidate{0.0f};
        switch (points[lower].interpolation) {
        case 0u: candidate = k0; break;
        case 1u: candidate = k0 + u * (k1 - k0); break;
        case 2u:
            candidate = k0 * (u - 1.0f) * (u - 1.0f) * (2.0f * u + 1.0f) +
                        k1 * u * u * (3.0f - 2.0f * u);
            break;
        case 3u: {
            const float scale = 1.0f / (p1 - p0);
            const float d0 = lower == 0u
                ? (k1 - k0) / (scale * (2.0f * p1 - 2.0f * p0))
                : 0.5f *
                      ((k1 - points[lower - 1u].value) /
                       (scale * (p1 - points[lower - 1u].position)));
            const float d1 = upper + 1u == points.size()
                ? (k1 - k0) / (scale * (2.0f * p1 - 2.0f * p0))
                : 0.5f *
                      ((points[upper + 1u].value - k0) /
                       (scale * (points[upper + 1u].position - p0)));
            candidate = k0 * (u - 1.0f) * (u - 1.0f) *
                            (2.0f * u + 1.0f) +
                        k1 * u * u * (3.0f - 2.0f * u) +
                        d0 * u * (u - 1.0f) * (u - 1.0f) +
                        d1 * u * u * (u - 1.0f);
            break;
        }
        default: candidate = 0.0f; break;
        }
        result = ite(parameter >= p0, clamp(candidate, 0.0f, 1.0f), result);
    }
    return ite(parameter >= points.back().position,
               clamp(points.back().value, 0.0f, 1.0f), result);
}

} // namespace

Expr<float> runtime_hash(span<const Expr<float>> arguments) noexcept {
    UInt state = 0x9e3779b9u;
    for (const Expr<float> argument : arguments) {
        state = state ^ (as<uint>(argument) + 0x9e3779b9u +
                         (state << 6u) + (state >> 2u));
        state = state ^ (state >> 16u);
        state = state * 0x7feb352du;
        state = state ^ (state >> 15u);
        state = state * 0x846ca68bu;
        state = state ^ (state >> 16u);
    }
    return cast<float>(state >> 8u) * (1.0f / 16777216.0f);
}

Expr<float> lower_expression(const XgenFloatExpressionProgram &program,
                             Expr<uint> index, Expr<uint> count,
                             const BufferFloat &inputs,
                             const BufferFloat &contexts) noexcept {
    vector<Expr<float>> values;
    values.reserve(program.instructions.size());
    for (const XgenFloatScalarInstruction &instruction : program.instructions) {
        auto operand = [&](std::size_t i) {
            return values[program.operands[instruction.operand_offset + i]];
        };
        Float value{0.0f};
        switch (instruction.op) {
        case XgenScalarOp::constant:
            value = instruction.immediate; break;
        case XgenScalarOp::input:
            value = inputs.read(index + count * instruction.auxiliary); break;
        case XgenScalarOp::negate: value = -operand(0u); break;
        case XgenScalarOp::logical_not:
            value = cast<float>(operand(0u) == 0.0f); break;
        case XgenScalarOp::add: value = operand(0u) + operand(1u); break;
        case XgenScalarOp::subtract: value = operand(0u) - operand(1u); break;
        case XgenScalarOp::multiply: value = operand(0u) * operand(1u); break;
        case XgenScalarOp::divide: value = operand(0u) / operand(1u); break;
        case XgenScalarOp::less:
            value = cast<float>(operand(0u) < operand(1u)); break;
        case XgenScalarOp::less_equal:
            value = cast<float>(operand(0u) <= operand(1u)); break;
        case XgenScalarOp::greater:
            value = cast<float>(operand(0u) > operand(1u)); break;
        case XgenScalarOp::greater_equal:
            value = cast<float>(operand(0u) >= operand(1u)); break;
        case XgenScalarOp::equal:
            value = cast<float>(operand(0u) == operand(1u)); break;
        case XgenScalarOp::not_equal:
            value = cast<float>(operand(0u) != operand(1u)); break;
        case XgenScalarOp::logical_and:
            value = cast<float>((operand(0u) != 0.0f) &
                                (operand(1u) != 0.0f));
            break;
        case XgenScalarOp::logical_or:
            value = cast<float>((operand(0u) != 0.0f) |
                                (operand(1u) != 0.0f));
            break;
        case XgenScalarOp::select:
            value = ite(operand(0u) != 0.0f, operand(1u), operand(2u)); break;
        case XgenScalarOp::hash: {
            vector<Expr<float>> arguments;
            arguments.reserve(instruction.operand_count);
            for (std::size_t i = 0u; i < instruction.operand_count; ++i) {
                arguments.emplace_back(operand(i));
            }
            value = runtime_hash(arguments);
            break;
        }
        case XgenScalarOp::random: {
            vector<Expr<float>> arguments;
            arguments.reserve(5u);
            arguments.emplace_back(contexts.read(index));
            arguments.emplace_back(contexts.read(index + count));
            arguments.emplace_back(contexts.read(index + count * 2u));
            arguments.emplace_back(instruction.immediate);
            if (instruction.operand_count == 1u ||
                instruction.operand_count == 3u) {
                arguments.emplace_back(operand(instruction.operand_count - 1u));
            }
            value = runtime_hash(arguments);
            if (instruction.operand_count >= 2u) {
                value = (operand(1u) - operand(0u)) * value + operand(0u);
            }
            break;
        }
        case XgenScalarOp::minimum: value = min(operand(0u), operand(1u)); break;
        case XgenScalarOp::maximum: value = max(operand(0u), operand(1u)); break;
        case XgenScalarOp::clamp:
            value = clamp(operand(0u), operand(1u), operand(2u)); break;
        case XgenScalarOp::fit:
            value = operand(3u) + (operand(0u) - operand(1u)) *
                (operand(4u) - operand(3u)) /
                (operand(2u) - operand(1u));
            break;
        case XgenScalarOp::ramp: {
            const XgenRamp ramp = program.ramps[instruction.auxiliary];
            value = lower_ramp(
                span{program.ramp_points}.subspan(
                    ramp.point_offset, ramp.point_count),
                contexts.read(index + count * 3u));
            break;
        }
        }
        values.emplace_back(value);
    }
    return values[program.result];
}

} // namespace nanoxgen::luisa_backend
