#pragma once

#include "nanoxgen/xgen_expression.h"

#include <luisa/core/stl/vector.h>
#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

[[nodiscard]] luisa::compute::Expr<float> runtime_hash(
    luisa::span<const luisa::compute::Expr<float>> arguments) noexcept;

// Record a fast-float XGen expression program into the current LuisaCompute
// callable/kernel. Inputs and contexts use structure-of-arrays layout:
// inputs[input * count + index], followed by u/v/face_seed/t context planes.
[[nodiscard]] luisa::compute::Expr<float> lower_expression(
    const XgenFloatExpressionProgram &program,
    luisa::compute::Expr<luisa::uint> index,
    luisa::compute::Expr<luisa::uint> count,
    const luisa::compute::BufferFloat &inputs,
    const luisa::compute::BufferFloat &contexts) noexcept;

} // namespace nanoxgen::luisa_backend
