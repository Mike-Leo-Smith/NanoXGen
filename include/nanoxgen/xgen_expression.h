#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen {

enum class XgenScalarOp : std::uint8_t {
    constant,
    input,
    negate,
    logical_not,
    add,
    subtract,
    multiply,
    divide,
    less,
    less_equal,
    greater,
    greater_equal,
    equal,
    not_equal,
    logical_and,
    logical_or,
    select,
    hash,
    random,
    minimum,
    maximum,
    clamp,
    fit,
    gamma,
    contrast,
    smoothstep,
    ramp,
};

struct XgenRampPoint {
    double position{};
    double value{};
    std::uint8_t interpolation{};
};

struct XgenFloatRampPoint {
    float position{};
    float value{};
    std::uint8_t interpolation{};
};

struct XgenRamp {
    std::uint32_t point_offset{};
    std::uint16_t point_count{};
};

// Instructions are SSA values. Their operands are instruction indices in the
// program's packed operands array and must always refer to an earlier value.
struct XgenScalarInstruction {
    XgenScalarOp op{XgenScalarOp::constant};
    std::uint32_t operand_offset{};
    std::uint16_t operand_count{};
    std::uint32_t auxiliary{};
    double immediate{};
};

struct XgenExpressionProgram {
    std::vector<XgenScalarInstruction> instructions;
    std::vector<std::uint32_t> operands;
    std::vector<std::string> inputs;
    std::vector<XgenRampPoint> ramp_points;
    std::vector<XgenRamp> ramps;
    std::uint32_t result{};
    std::uint32_t expression_seed{};
    std::uint32_t random_call_count{};
};

// Device/runtime form of the expression IR. Unlike XgenExpressionProgram,
// this type contains no double fields. Construct it once after strict Autodesk
// calibration, then retain/JIT this compact form for rendering.
struct XgenFloatScalarInstruction {
    XgenScalarOp op{XgenScalarOp::constant};
    std::uint32_t operand_offset{};
    std::uint16_t operand_count{};
    std::uint32_t auxiliary{};
    float immediate{};
};

struct XgenFloatExpressionProgram {
    std::vector<XgenFloatScalarInstruction> instructions;
    std::vector<std::uint32_t> operands;
    std::vector<std::string> inputs;
    std::vector<XgenFloatRampPoint> ramp_points;
    std::vector<XgenRamp> ramps;
    std::uint32_t result{};
    std::uint32_t expression_seed{};
    std::uint32_t random_call_count{};
};

struct XgenExpressionCompileOptions {
    std::string expression_name;
    std::string object_type;
    std::size_t max_source_bytes{1024u * 1024u};
    std::size_t max_tokens{100000u};
    std::size_t max_instructions{100000u};
    std::size_t max_locals{4096u};
    std::size_t max_ramp_points{4096u};
};

struct XgenExpressionContext {
    std::span<const double> inputs;
    double u{};
    double v{};
    double face_seed{};
    double t{};
};

// Throughput-oriented runtime context. This path deliberately uses only
// float/uint operations so the same IR is efficient on GPUs with weak FP64.
// Classic root planners may provide the exact SeExpr prefix for (u,v,face)
// so rand() remains Autodesk-compatible without device FP64.
struct XgenExpressionFloatContext {
    std::span<const float> inputs;
    float u{};
    float v{};
    float face_seed{};
    float t{};
    std::uint32_t random_prefix{};
    bool has_random_prefix{};
};

[[nodiscard]] XgenExpressionProgram compile_xgen_scalar_expression(
    std::string_view source, const XgenExpressionCompileOptions &options = {});

[[nodiscard]] XgenFloatExpressionProgram make_xgen_float_expression_program(
    const XgenExpressionProgram &program);

[[nodiscard]] double evaluate_xgen_scalar_expression(
    const XgenExpressionProgram &program, const XgenExpressionContext &context);

// Allocation-free exact CPU evaluation. Scratch must contain at least
// program.instructions.size() doubles.
[[nodiscard]] double evaluate_xgen_scalar_expression(
    const XgenExpressionProgram &program, const XgenExpressionContext &context,
    std::span<double> scratch);

[[nodiscard]] float evaluate_xgen_scalar_expression_float(
    const XgenFloatExpressionProgram &program,
    const XgenExpressionFloatContext &context);

// Allocation-free CPU evaluation for hot runtime-plan loops. Scratch must
// contain at least program.instructions.size() floats.
[[nodiscard]] float evaluate_xgen_scalar_expression_float(
    const XgenFloatExpressionProgram &program,
    const XgenExpressionFloatContext &context,
    std::span<float> scratch);

[[nodiscard]] std::uint32_t xgen_runtime_hash32(
    std::span<const float> arguments) noexcept;
[[nodiscard]] std::uint32_t xgen_runtime_hash_component(
    float argument) noexcept;
[[nodiscard]] float xgen_runtime_hash(std::span<const float> arguments) noexcept;
[[nodiscard]] std::uint32_t xgen_seexpr_component(double argument) noexcept;
[[nodiscard]] std::uint32_t xgen_seexpr_hash_prefix(
    std::span<const double> arguments) noexcept;
[[nodiscard]] std::uint32_t xgen_seexpr_hash_finish(
    std::uint32_t state) noexcept;
[[nodiscard]] float xgen_seexpr_hash_finish_float(
    std::uint32_t state) noexcept;
[[nodiscard]] float xgen_runtime_face_seed(
    std::uint32_t description_id, std::string_view patch_name,
    std::uint32_t face_id) noexcept;

// Exact scalar hash used by the public SeExpr implementation bundled with
// XGen. These helpers make expression and face seed construction explicit for
// CPU/GPU backends and for Autodesk differential tests.
[[nodiscard]] double xgen_seexpr_hash(std::span<const double> arguments) noexcept;
[[nodiscard]] std::uint32_t xgen_string_seed(std::string_view text) noexcept;
[[nodiscard]] std::uint32_t xgen_expression_seed(
    std::string_view expression_name, std::string_view object_type) noexcept;
[[nodiscard]] double xgen_face_seed(
    std::uint32_t description_id, std::string_view patch_name,
    std::uint32_t face_id) noexcept;

} // namespace nanoxgen
