#pragma once

#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

struct ClassicFloatRuntimeLuisaContext {
    luisa::compute::Expr<luisa::uint> id;
    luisa::compute::Expr<float> u;
    luisa::compute::Expr<float> v;
    luisa::compute::Expr<float> face_seed;
    luisa::compute::Expr<float> c_length;
    luisa::compute::Expr<float> c_width;
    luisa::compute::Expr<float> t;
};

// Bind a Classic runtime expression directly to values in the surrounding
// Luisa kernel. Only XgenFloatExpressionProgram is accepted by this API.
[[nodiscard]] luisa::compute::Expr<float> lower_classic_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeLuisaContext &context);

} // namespace nanoxgen::luisa_backend
