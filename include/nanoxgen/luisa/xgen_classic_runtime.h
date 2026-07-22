#pragma once

#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

using ClassicRuntimePrimitiveKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::Buffer<float>,
    luisa::compute::Buffer<luisa::float4>)>;
using ClassicRuntimeCutKernel = ClassicRuntimePrimitiveKernel;
using ClassicRuntimeClumpKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::Buffer<float>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::Buffer<luisa::uint>)>;
using ClassicRuntimeNoiseKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::Buffer<float>,
    luisa::compute::Buffer<luisa::float3>,
    luisa::compute::Buffer<luisa::float4>)>;
using ClassicRuntimeWidthKernel = luisa::compute::Kernel1D<void(
    luisa::compute::Buffer<luisa::float4>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::Buffer<float>,
    luisa::compute::Buffer<luisa::float4>)>;

struct ClassicFloatRuntimeLuisaContext {
    luisa::compute::Expr<luisa::uint> id;
    luisa::compute::Expr<float> u;
    luisa::compute::Expr<float> v;
    luisa::compute::Expr<float> face_seed;
    luisa::compute::Expr<float> c_length;
    luisa::compute::Expr<float> c_width;
    luisa::compute::Expr<float> t;
    luisa::compute::Expr<luisa::uint> random_prefix;
    bool has_random_prefix{};
    const luisa::compute::BufferVar<float> *ptex_values{};
    luisa::compute::Expr<luisa::uint> ptex_offset{0u};
    std::uint32_t ptex_stride{};
};

// Bind a Classic runtime expression directly to values in the surrounding
// Luisa kernel. Only XgenFloatExpressionProgram is accepted by this API.
[[nodiscard]] luisa::compute::Expr<float> lower_classic_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeLuisaContext &context);

// Kernels for a zero-host-copy Classic postprocess pipeline. Packed points are
// float4(position, radius); roots is a byte view of RootSample records;
// root_runtime is interleaved uint2(primitiveId, exactSeExprPrefix); state is
// one float4(cLength, cWidth, taper, taperStart) per strand.
[[nodiscard]] ClassicRuntimePrimitiveKernel make_classic_runtime_primitive_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale = 1.0f);

[[nodiscard]] ClassicRuntimeCutKernel make_classic_runtime_cut_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatCutModule &cut,
    std::uint32_t cvs_per_strand);

// guide_axes stores float4(position, 0) guide-major; guide_frames stores two
// float4 records per guide as (normal.xyz, u) and (tangent.xyz, v);
// guide_runtime stores uint2(faceId, exactSeExprPrefix); strand_guides stores
// one guide index (or kInvalidIndex) per strand.
[[nodiscard]] ClassicRuntimeClumpKernel make_classic_runtime_clump_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatClumpModule &clump,
    std::uint32_t cvs_per_strand,
    std::uint32_t guide_count);

[[nodiscard]] ClassicRuntimeNoiseKernel make_classic_runtime_noise_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatNoiseModule &noise,
    std::uint32_t cvs_per_strand);

[[nodiscard]] ClassicRuntimeWidthKernel make_classic_runtime_width_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale = 1.0f);

} // namespace nanoxgen::luisa_backend
