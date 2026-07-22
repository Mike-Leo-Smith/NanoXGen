#include "nanoxgen/luisa/xgen_expression.h"

#include <luisa/dsl/constant.h>

#include <array>

namespace nanoxgen::luisa_backend {

using namespace luisa;
using namespace luisa::compute;

namespace {

constexpr std::array<uint, 256u> permutation{
    148,201,203,34,85,225,163,200,174,137,51,24,19,252,107,173,110,251,149,69,180,152,
    141,132,22,20,147,219,37,46,154,114,59,49,155,161,239,77,47,10,70,227,53,235,
    30,188,143,73,88,193,214,194,18,120,176,36,212,84,211,142,167,57,153,71,159,151,
    126,115,229,124,172,101,79,183,32,38,68,11,67,109,221,3,4,61,122,94,72,117,
    12,240,199,76,118,5,48,197,128,62,119,89,14,45,226,195,80,50,40,192,60,65,
    166,106,90,215,213,232,250,207,104,52,182,29,157,103,242,97,111,17,8,175,254,108,
    208,224,191,112,105,187,43,56,185,243,196,156,246,249,184,7,135,6,158,82,130,234,
    206,255,160,236,171,230,42,98,54,74,209,205,33,177,15,138,178,44,116,96,140,253,
    233,125,21,133,136,86,245,58,23,1,75,165,92,217,39,0,218,91,179,55,238,170,
    134,83,25,189,216,100,129,150,241,210,123,99,2,164,16,220,121,139,168,64,190,9,
    31,228,95,247,244,81,102,145,204,146,26,87,113,198,181,127,237,169,28,93,27,41,
    231,248,78,162,13,186,63,66,131,202,35,144,222,223};

Expr<uint> runtime_hash_component(Expr<float> argument) noexcept {
    constexpr luisa::ulong constant_mantissa = 0x001114580b45d474ull;
    constexpr uint constant_low = static_cast<uint>(constant_mantissa);
    constexpr uint constant_high = static_cast<uint>(constant_mantissa >> 32u);
    UInt bits = as<uint>(argument);
    UInt absolute = bits & 0x7fffffffu;
    UInt float_exponent = absolute >> 23u;
    UInt mantissa = ite(float_exponent == 0u,
                        absolute & 0x7fffffu,
                        (absolute & 0x7fffffu) | 0x800000u);
    ULong p0 = cast<luisa::ulong>(mantissa) *
               static_cast<luisa::ulong>(constant_low);
    ULong p1 = cast<luisa::ulong>(mantissa) *
               static_cast<luisa::ulong>(constant_high);
    ULong low = p0 + (p1 << 32u);
    ULong high = (p1 >> 32u) + cast<luisa::ulong>(low < p0);
    UInt high_word = cast<uint>(high);
    UInt low_high_word = cast<uint>(low >> 32u);
    UInt low_word = cast<uint>(low);
    UInt length = ite(high_word != 0u,
                      96u - clz(high_word),
                      ite(low_high_word != 0u,
                          64u - clz(low_high_word),
                          32u - clz(low_word)));
    UInt shift = length - 32u;
    UInt top = cast<uint>((low >> shift) | (high << (64u - shift)));
    ULong remainder = low & ((1ull << shift) - 1ull);
    ULong ceil_div_2_32 = ((low >> 32u) | (high << 32u)) +
                          cast<luisa::ulong>((low & 0xffffffffull) != 0ull);
    UInt fraction = top - cast<uint>(remainder < ceil_div_2_32);
    UInt value_exponent = ite(float_exponent == 0u,
                              static_cast<uint>(-149),
                              float_exponent - 150u);
    UInt exponent = length + value_exponent - 49u;
    UInt component = ite((bits >> 31u) != 0u, 0u - fraction, fraction) ^ exponent;
    return ite(absolute != 0u, component, 0u);
}

Expr<uint> runtime_hash_finish(Expr<uint> input) noexcept {
    UInt state = input;
    state = state ^ (state >> 11u);
    state = state ^ ((state << 7u) & 0x9d2c5680u);
    state = state ^ ((state << 15u) & 0xefc60000u);
    state = state ^ (state >> 18u);
    Constant<uint> lookup{permutation};
    UInt b3 = lookup[state & 0xffu];
    UInt b2 = lookup[((state >> 8u) + b3) & 0xffu];
    UInt b1 = lookup[((state >> 16u) + b2) & 0xffu];
    UInt b0 = lookup[((state >> 24u) + b1) & 0xffu];
    return b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u);
}

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
    UInt state = 0u;
    for (const Expr<float> argument : arguments) {
        state = state * 1664525u + runtime_hash_component(argument) +
                1013904223u;
    }
    return cast<float>(runtime_hash_finish(state)) * 0x1p-32f;
}

Expr<float> lower_expression(const XgenFloatExpressionProgram &program,
                             span<const Expr<float>> inputs,
                             Expr<float> u, Expr<float> v,
                             Expr<float> face_seed, Expr<float> t,
                             Expr<uint> random_prefix,
                             bool has_random_prefix) noexcept {
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
            value = inputs[instruction.auxiliary]; break;
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
            const bool explicit_seed = instruction.operand_count == 1u ||
                                       instruction.operand_count == 3u;
            if (has_random_prefix) {
                UInt state = random_prefix * 1664525u +
                             instruction.auxiliary + 1013904223u;
                if (explicit_seed) {
                    state = state * 1664525u +
                            runtime_hash_component(
                                operand(instruction.operand_count - 1u)) +
                            1013904223u;
                }
                value = cast<float>(runtime_hash_finish(state)) * 0x1p-32f;
            } else {
                vector<Expr<float>> arguments;
                arguments.reserve(5u);
                arguments.emplace_back(u);
                arguments.emplace_back(v);
                arguments.emplace_back(face_seed);
                arguments.emplace_back(instruction.immediate);
                if (explicit_seed) {
                    arguments.emplace_back(
                        operand(instruction.operand_count - 1u));
                }
                value = runtime_hash(arguments);
            }
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
        case XgenScalarOp::gamma:
            value = pow(operand(0u), 1.0f / operand(1u));
            break;
        case XgenScalarOp::contrast: {
            const Float x = operand(0u);
            const Float edge = ite(x < 0.5f, 2.0f * x, 2.0f - 2.0f * x);
            const Float exponent = -log2(edge);
            const Float shaped =
                0.5f * pow(1.0f - operand(1u), exponent);
            value = ite(x < 0.5f, shaped, 1.0f - shaped);
            break;
        }
        case XgenScalarOp::smoothstep: {
            const Float t = clamp(
                (operand(0u) - operand(1u)) /
                    (operand(2u) - operand(1u)),
                0.0f, 1.0f);
            value = t * t * (3.0f - 2.0f * t);
            break;
        }
        case XgenScalarOp::ramp: {
            const XgenRamp ramp = program.ramps[instruction.auxiliary];
            value = lower_ramp(
                span{program.ramp_points}.subspan(
                    ramp.point_offset, ramp.point_count),
                t);
            break;
        }
        }
        values.emplace_back(value);
    }
    return values[program.result];
}

Expr<float> lower_expression(const XgenFloatExpressionProgram &program,
                             span<const Expr<float>> inputs,
                             Expr<float> u, Expr<float> v,
                             Expr<float> face_seed, Expr<float> t) noexcept {
    return lower_expression(program, inputs, u, v, face_seed, t, 0u, false);
}

Expr<float> lower_expression(const XgenFloatExpressionProgram &program,
                             Expr<uint> index, Expr<uint> count,
                             const BufferFloat &inputs,
                             const BufferFloat &contexts) noexcept {
    vector<Expr<float>> values;
    values.reserve(program.inputs.size());
    for (std::size_t input = 0u; input < program.inputs.size(); ++input) {
        values.emplace_back(inputs.read(
            index + count * static_cast<uint>(input)));
    }
    return lower_expression(
        program, values, contexts.read(index),
        contexts.read(index + count),
        contexts.read(index + count * 2u),
        contexts.read(index + count * 3u));
}

} // namespace nanoxgen::luisa_backend
