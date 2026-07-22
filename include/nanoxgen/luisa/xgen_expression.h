#pragma once

#include "nanoxgen/xgen_expression.h"

#include <luisa/core/stl/vector.h>
#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

[[nodiscard]] luisa::compute::Expr<float> runtime_hash(
    luisa::span<const luisa::compute::Expr<float>> arguments) noexcept;

// Direct expression form used when a larger JIT kernel already owns the
// variables. No device-side interpreter or temporary input buffer is created.
[[nodiscard]] luisa::compute::Expr<float> lower_expression(
    const XgenFloatExpressionProgram &program,
    luisa::span<const luisa::compute::Expr<float>> inputs,
    luisa::compute::Expr<float> u,
    luisa::compute::Expr<float> v,
    luisa::compute::Expr<float> face_seed,
    luisa::compute::Expr<float> t) noexcept;

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
