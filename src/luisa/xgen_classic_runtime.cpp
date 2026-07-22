#include "nanoxgen/luisa/xgen_classic_runtime.h"

#include "nanoxgen/luisa/xgen_expression.h"

#include <luisa/core/stl/vector.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace nanoxgen::luisa_backend {

using namespace luisa;
using namespace luisa::compute;
using ::operator+;
using ::operator-;
using ::operator*;
using ::operator/;

namespace {

static_assert(sizeof(PackedCurvePoint) == sizeof(float4));
static_assert(sizeof(RootSample) == 44u);
static_assert(offsetof(RootSample, uv) == 24u);
static_assert(offsetof(RootSample, triangle_index) == 32u);

ClassicFloatRuntimeLuisaContext make_context(
    const ClassicFloatRuntimePlan &plan, Expr<uint> strand,
    const ByteBufferVar &roots, Expr<float> c_length,
    Expr<float> c_width, Expr<float> t) noexcept {
    const UInt root_offset = strand * static_cast<uint>(sizeof(RootSample));
    const Float2 uv = roots.read<float2>(
        root_offset + static_cast<uint>(offsetof(RootSample, uv)));
    const UInt triangle = roots.read<uint>(
        root_offset + static_cast<uint>(offsetof(RootSample, triangle_index)));
    const std::array<Expr<float>, 3u> seed_arguments{
        static_cast<float>(plan.description_id),
        static_cast<float>(xgen_string_seed(plan.description_name)),
        cast<float>(triangle)};
    return {strand, uv.x, uv.y, runtime_hash(seed_arguments),
            c_length, c_width, t};
}

Float polyline_length(const BufferFloat4 &points, Expr<uint> first,
                      std::uint32_t cvs_per_strand) noexcept {
    Float length{0.0f};
    Float3 previous = points.read(first).xyz();
    for (std::uint32_t cv = 1u; cv < cvs_per_strand; ++cv) {
        const Float3 current = points.read(first + cv).xyz();
        length += sqrt(dot(current - previous, current - previous));
        previous = current;
    }
    return length;
}

} // namespace

Expr<float> lower_classic_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeLuisaContext &context) {
    vector<Expr<float>> inputs;
    inputs.reserve(expression.program.inputs.size());
    for (const std::string &input : expression.program.inputs) {
        if (input == "id") {
            inputs.emplace_back(cast<float>(context.id));
        } else if (input == "cLength") {
            inputs.emplace_back(context.c_length);
        } else if (input == "cWidth") {
            inputs.emplace_back(context.c_width);
        } else {
            throw std::runtime_error(
                "Classic Luisa runtime variable is not bound: $" + input);
        }
    }
    return lower_expression(expression.program, inputs, context.u, context.v,
                            context.face_seed, context.t);
}

ClassicRuntimePrimitiveKernel make_classic_runtime_primitive_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale) {
    if (cvs_per_strand < 2u) {
        throw std::invalid_argument("Classic Luisa runtime needs at least two CVs");
    }
    if (!std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument("Classic Luisa radius scale is invalid");
    }
    return Kernel1D{[=, &plan](BufferFloat4 source, BufferFloat4 destination,
                               ByteBufferVar roots,
                               BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const Float4 root_point = source.read(first);
        const Float base_length = polyline_length(source, first, cvs_per_strand);
        const auto base_context = make_context(
            plan, strand, roots, base_length,
            radius_scale > 0.0f ? 2.0f * root_point.w / radius_scale : 0.0f,
            0.0f);
        Float length_scale{1.0f};
        if (plan.length) {
            length_scale = lower_classic_runtime_expression(
                *plan.length, base_context);
        }
        const Float c_length = base_length * length_scale;
        const auto length_context = make_context(
            plan, strand, roots, c_length, base_context.c_width, 0.0f);
        const Float c_width = plan.width
            ? lower_classic_runtime_expression(*plan.width, length_context)
            : length_context.c_width;
        const auto width_context = make_context(
            plan, strand, roots, c_length, c_width, 0.0f);
        Float taper{0.0f};
        if (plan.taper) {
            taper = lower_classic_runtime_expression(*plan.taper, width_context);
        }
        Float taper_start{0.0f};
        if (plan.taper_start) {
            taper_start = lower_classic_runtime_expression(
                *plan.taper_start, width_context);
        }
        states.write(strand, make_float4(
            c_length, c_width, taper, taper_start));
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const Float4 source_point = source.read(first + cv);
            const Float3 position = root_point.xyz() +
                (source_point.xyz() - root_point.xyz()) * length_scale;
            destination.write(first + cv,
                              make_float4(position, source_point.w));
        }
    }};
}

ClassicRuntimeCutKernel make_classic_runtime_cut_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatCutModule &cut,
    std::uint32_t cvs_per_strand) {
    if (cvs_per_strand < 2u || cut.rebuild_type != 1u) {
        throw std::invalid_argument(
            "Classic Luisa Cut requires rebuildType 1 and at least two CVs");
    }
    return Kernel1D{[=, &plan, &cut](
                        BufferFloat4 source, BufferFloat4 destination,
                        ByteBufferVar roots, BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const Float input_length = polyline_length(source, first, cvs_per_strand);
        const Float4 state = states.read(strand);
        const auto context = make_context(
            plan, strand, roots, input_length, state.y, 0.0f);
        const Float amount = lower_classic_runtime_expression(cut.amount, context);
        const Float remaining = clamp(
            input_length - max(amount, 0.0f), 0.0f, input_length);
        states.write(strand, make_float4(
            remaining, state.y, state.z, state.w));
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const Float distance = remaining *
                (static_cast<float>(cv) /
                 static_cast<float>(cvs_per_strand - 1u));
            Float cumulative{0.0f};
            Bool found{false};
            Float3 sampled = source.read(first).xyz();
            Float source_radius = source.read(first).w;
            for (std::uint32_t segment = 0u;
                 segment + 1u < cvs_per_strand; ++segment) {
                const Float4 a = source.read(first + segment);
                const Float4 b = source.read(first + segment + 1u);
                const Float segment_length = sqrt(dot(
                    b.xyz() - a.xyz(), b.xyz() - a.xyz()));
                const Bool use = !found &
                    (distance <= cumulative + segment_length);
                const Float weight = (distance - cumulative) /
                    max(segment_length, 1.0e-20f);
                sampled = ite(use, lerp(a.xyz(), b.xyz(), weight), sampled);
                source_radius = ite(use, lerp(a.w, b.w, weight), source_radius);
                found = found | use;
                cumulative += segment_length;
            }
            destination.write(first + cv,
                              make_float4(sampled, source_radius));
        }
    }};
}

ClassicRuntimeWidthKernel make_classic_runtime_width_kernel(
    const ClassicFloatRuntimePlan &plan,
    std::uint32_t cvs_per_strand,
    float radius_scale) {
    if (cvs_per_strand < 2u || !std::isfinite(radius_scale) ||
        radius_scale < 0.0f) {
        throw std::invalid_argument("Classic Luisa width kernel arguments are invalid");
    }
    return Kernel1D{[=, &plan](BufferFloat4 points, ByteBufferVar roots,
                               BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const Float c_length = polyline_length(points, first, cvs_per_strand);
        const Float4 state = states.read(strand);
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const float t_constant = static_cast<float>(cv) /
                                     static_cast<float>(cvs_per_strand - 1u);
            const Float t = t_constant;
            Float scale{1.0f};
            scale *= ite((t > state.w) & (state.w < 1.0f),
                         1.0f - state.z * ((t - state.w) /
                             max(1.0f - state.w, 1.0e-20f)),
                         1.0f);
            const auto context = make_context(
                plan, strand, roots, c_length, state.y, t);
            if (plan.width_ramp) {
                scale *= lower_classic_runtime_expression(
                    *plan.width_ramp, context);
            }
            const Float4 point = points.read(first + cv);
            points.write(first + cv, make_float4(
                point.xyz(), 0.5f * state.y * scale * radius_scale));
        }
    }};
}

} // namespace nanoxgen::luisa_backend
