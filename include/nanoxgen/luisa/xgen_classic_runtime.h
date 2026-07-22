#pragma once

#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

using ClassicRuntimePrimitiveKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::float4>)>;
using ClassicRuntimeCutKernel = ClassicRuntimePrimitiveKernel;
using ClassicRuntimeWidthKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::float4>)>;

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

// Kernels for a zero-host-copy Classic postprocess pipeline. Packed points are
// float4(position, radius); roots is a byte view of RootSample records; state
// is one float4(cLength, cWidth, taper, taperStart) per strand.
[[nodiscard]] ClassicRuntimePrimitiveKernel make_classic_runtime_primitive_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale = 1.0f);

[[nodiscard]] ClassicRuntimeCutKernel make_classic_runtime_cut_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatCutModule &cut,
    std::uint32_t cvs_per_strand);

[[nodiscard]] ClassicRuntimeWidthKernel make_classic_runtime_width_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale = 1.0f);

} // namespace nanoxgen::luisa_backend
