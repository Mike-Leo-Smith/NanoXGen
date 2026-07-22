#include "nanoxgen/luisa/xgen_classic_runtime.h"

#include "nanoxgen/luisa/xgen_expression.h"
#include "nanoxgen/seexpr_noise_table.h"

#include <luisa/core/stl/vector.h>
#include <luisa/dsl/constant.h>

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
using ::operator==;

namespace {

static_assert(sizeof(PackedCurvePoint) == sizeof(float4));
static_assert(sizeof(RootSample) == 48u);
static_assert(offsetof(RootSample, uv) == 24u);
static_assert(offsetof(RootSample, triangle_index) == 32u);
static_assert(offsetof(RootSample, surface_face_id) == 44u);

ClassicFloatRuntimeLuisaContext make_context(
    const ClassicFloatRuntimePlan &plan, Expr<uint> strand,
    const ByteBufferVar &roots, const BufferUInt &root_runtime,
    Expr<float> c_length,
    Expr<float> c_width, Expr<float> t) noexcept {
    const UInt root_offset = strand * static_cast<uint>(sizeof(RootSample));
    const Float2 uv = roots.read<float2>(
        root_offset + static_cast<uint>(offsetof(RootSample, uv)));
    const UInt face_id = roots.read<uint>(
        root_offset + static_cast<uint>(offsetof(RootSample, surface_face_id)));
    const std::array<Expr<float>, 3u> seed_arguments{
        static_cast<float>(plan.description_id),
        static_cast<float>(xgen_string_seed(plan.description_name)),
        cast<float>(face_id)};
    return {root_runtime.read(strand * 2u), uv.x, uv.y,
            runtime_hash(seed_arguments), c_length, c_width, t,
            root_runtime.read(strand * 2u + 1u), true};
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

Float3 xgen_curve_eval(const BufferFloat4 &points, Expr<uint> first,
                       Expr<float> parameter,
                       std::uint32_t cvs_per_strand) noexcept {
    const Float scaled = parameter * static_cast<float>(cvs_per_strand - 1u);
    const UInt span = min(cast<uint>(scaled), cvs_per_strand - 2u);
    const Float f = scaled - cast<float>(span);
    const Float f2 = f * f;
    const Float f3 = f2 * f;
    const Float one_minus_f = 1.0f - f;
    const Float b0 = one_minus_f * one_minus_f * one_minus_f;
    const Float b1 = 3.0f * f3 - 6.0f * f2 + 4.0f;
    const Float b2 = -3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f;
    const Float b3 = f3;
    const Float3 first_point = points.read(first).xyz();
    const Float3 last_point = points.read(first + cvs_per_strand - 1u).xyz();
    const Float3 p1 = points.read(first + span).xyz();
    const Float3 p2 = points.read(first + span + 1u).xyz();
    const Float3 p0 = ite(span == 0u,
                          first_point * 2.0f - points.read(first + 1u).xyz(),
                          points.read(first + max(span, 1u) - 1u).xyz());
    const Float3 p3 = ite(span + 2u == cvs_per_strand,
                          last_point * 2.0f -
                              points.read(first + cvs_per_strand - 2u).xyz(),
                          points.read(first +
                              min(span + 2u, cvs_per_strand - 1u)).xyz());
    const Float3 cubic =
        (p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3) * (1.0f / 6.0f);
    return ite(parameter < 1.0e-7f, first_point,
               ite(parameter > 1.0f - 1.0e-7f, last_point, cubic));
}

Float xgen_noise(const Constant<float> &gradients,
                 Expr<float3> sample) noexcept {
    const Float3 floored = floor(sample);
    const Int3 cell = cast<int3>(floored);
    const Float3 weights = sample - floored;
    vector<Float> values;
    values.reserve(8u);
    for (std::uint32_t corner = 0u; corner < 8u; ++corner) {
        const int ox = static_cast<int>(corner & 1u);
        const int oy = static_cast<int>((corner >> 1u) & 1u);
        const int oz = static_cast<int>((corner >> 2u) & 1u);
        UInt seed = 0u;
        seed = seed * 1664525u + cast<uint>(cell.x + ox) + 1013904223u;
        seed = seed * 1664525u + cast<uint>(cell.y + oy) + 1013904223u;
        seed = seed * 1664525u + cast<uint>(cell.z + oz) + 1013904223u;
        seed = seed ^ (seed >> 11u);
        seed = seed ^ ((seed << 7u) & 0x9d2c5680u);
        seed = seed ^ ((seed << 15u) & 0xefc60000u);
        seed = seed ^ (seed >> 18u);
        const UInt index = ((((seed & 0x00ff0000u) >> 4u) +
                             (seed & 0xffu)) & 0xffu) * 3u;
        const Float3 gradient = make_float3(
            gradients[index], gradients[index + 1u], gradients[index + 2u]);
        values.emplace_back(
            gradient.x * (weights.x - static_cast<float>(ox)) +
            gradient.y * (weights.y - static_cast<float>(oy)) +
            gradient.z * (weights.z - static_cast<float>(oz)));
    }
    const Float3 alphas = weights * weights * weights *
        (weights * (weights * 6.0f - 15.0f) + 10.0f);
    for (int dimension = 2; dimension >= 0; --dimension) {
        const int count = 1 << dimension;
        for (int value = 0; value < count; ++value) {
            const int index = value * (1 << (3 - dimension));
            const int axis = 2 - dimension;
            const int other = index + (1 << axis);
            values[index] = (1.0f - alphas[axis]) * values[index] +
                            alphas[axis] * values[other];
        }
    }
    return 0.5f * values[0] + 0.5f;
}

Float3 safe_normalize(Expr<float3> value,
                      Expr<float3> fallback) noexcept {
    const Float length_squared = dot(value, value);
    return ite(length_squared > 1.0e-20f,
               value / sqrt(max(length_squared, 1.0e-20f)), fallback);
}

Float3 rotate_by(Expr<float3> value, Expr<float3> axis,
                 Expr<float> angle) noexcept {
    const Float cosine = cos(angle);
    const Float sine = sin(angle);
    return value * cosine + cross(axis, value) * sine +
           axis * dot(axis, value) * (1.0f - cosine);
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
                            context.face_seed, context.t,
                            context.random_prefix,
                            context.has_random_prefix);
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
                               BufferUInt root_runtime,
                               BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const Float4 root_point = source.read(first);
        const Float base_length = polyline_length(source, first, cvs_per_strand);
        const auto base_context = make_context(
            plan, strand, roots, root_runtime, base_length,
            radius_scale > 0.0f ? 2.0f * root_point.w / radius_scale : 0.0f,
            0.0f);
        Float length_scale{1.0f};
        if (plan.length) {
            length_scale = lower_classic_runtime_expression(
                *plan.length, base_context);
        }
        const Float c_length = base_length * length_scale;
        const auto length_context = make_context(
            plan, strand, roots, root_runtime, c_length,
            base_context.c_width, 0.0f);
        const Float c_width = plan.width
            ? lower_classic_runtime_expression(*plan.width, length_context)
            : length_context.c_width;
        const auto width_context = make_context(
            plan, strand, roots, root_runtime, c_length, c_width, 0.0f);
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
                        ByteBufferVar roots, BufferUInt root_runtime,
                        BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const Float input_length = polyline_length(source, first, cvs_per_strand);
        const Float4 state = states.read(strand);
        const auto context = make_context(
            plan, strand, roots, root_runtime,
            input_length, state.y, 0.0f);
        const Float amount = lower_classic_runtime_expression(cut.amount, context);
        const Float cut_amount = max(amount, 0.0f);
        const float search_step = 1.0f /
            (2.0f * static_cast<float>(cvs_per_strand) + 4.0f);
        Float cut_parameter{1.0f};
        Float previous_parameter{1.0f};
        Float3 previous = source.read(first + cvs_per_strand - 1u).xyz();
        Float accumulated{0.0f};
        Bool finished = cut_amount < 1.0e-10f;
        for (std::uint32_t iteration = 0u;
             iteration < 2u * cvs_per_strand + 5u; ++iteration) {
            const Float parameter = max(previous_parameter - search_step, 0.0f);
            const Float3 current = xgen_curve_eval(
                source, first, parameter, cvs_per_strand);
            const Float segment_length = sqrt(dot(
                previous - current, previous - current));
            const Float next_accumulated = accumulated + segment_length;
            const Bool hit = !finished & (next_accumulated >= cut_amount) &
                             (segment_length > 0.0f);
            cut_parameter = ite(
                hit,
                parameter + ((next_accumulated - cut_amount) /
                    segment_length) * (previous_parameter - parameter),
                cut_parameter);
            const Bool at_root = !finished & !hit &
                                 (parameter <= 1.0e-10f);
            cut_parameter = ite(at_root, 0.0f, cut_parameter);
            const Bool was_finished = finished;
            finished = finished | hit | at_root;
            accumulated = ite(was_finished, accumulated, next_accumulated);
            previous_parameter = ite(was_finished, previous_parameter, parameter);
            previous = ite(was_finished, previous, current);
        }
        Float3 previous_output = source.read(first).xyz();
        Float rebuilt_length{0.0f};
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const Float parameter = cut_parameter *
                (static_cast<float>(cv) /
                 static_cast<float>(cvs_per_strand - 1u));
            const Float3 sampled = xgen_curve_eval(
                source, first, parameter, cvs_per_strand);
            if (cv != 0u) {
                rebuilt_length += sqrt(dot(
                    sampled - previous_output, sampled - previous_output));
            }
            previous_output = sampled;
            destination.write(first + cv,
                              make_float4(sampled, source.read(first + cv).w));
        }
        states.write(strand, make_float4(
            ite(cut_parameter < 1.0e-4f, -1.0f, rebuilt_length),
            state.y, state.z, state.w));
    }};
}

ClassicRuntimeNoiseKernel make_classic_runtime_noise_kernel(
    const ClassicFloatRuntimePlan &plan,
    const ClassicFloatNoiseModule &noise,
    std::uint32_t cvs_per_strand) {
    if (cvs_per_strand < 2u || noise.mode != 0u) {
        throw std::invalid_argument(
            "Classic Luisa Noise requires mode 0 and at least two CVs");
    }
    return Kernel1D{[=, &plan, &noise](
                        BufferFloat4 source, BufferFloat4 destination,
                        ByteBufferVar roots, BufferUInt root_runtime,
                        BufferFloat3 surface_tangents,
                        BufferFloat4 states) noexcept {
        set_block_size(128u, 1u, 1u);
        Constant<float> gradients{
            span<const float>{
                reinterpret_cast<const float *>(
                    detail::kSeExprNoiseGradients),
                256u * 3u}};
        const UInt strand = dispatch_id().x;
        const UInt first = strand * cvs_per_strand;
        const UInt root_offset = strand * static_cast<uint>(sizeof(RootSample));
        const Float3 surface_normal = roots.read<float3>(
            root_offset + static_cast<uint>(offsetof(RootSample, normal)));
        const Float4 state = states.read(strand);
        const Float original_length = polyline_length(
            source, first, cvs_per_strand);
        auto context = make_context(
            plan, strand, roots, root_runtime,
            original_length, state.y, 0.0f);
        const Float mask = clamp(
            lower_classic_runtime_expression(noise.mask, context), 0.0f, 1.0f);
        const Float frequency = max(
            lower_classic_runtime_expression(noise.frequency, context), 0.0f);
        const Float correlation = clamp(
            lower_classic_runtime_expression(noise.correlation, context) *
                0.01f,
            0.0f, 1.0f);
        const Float preserve = clamp(
            lower_classic_runtime_expression(noise.preserve_length, context) *
                0.01f,
            0.0f, 1.0f);
        const Float effective_frequency = ite(
            original_length > 0.0f,
            max(0.5f / max(original_length, 1.0e-20f), frequency),
            frequency);
        const Float decorrelation = 1.0f - correlation;
        const Float domain_scale =
            100.0f * decorrelation * decorrelation;
        const Float3 root = source.read(first).xyz();
        const Float3 domain =
            (root + make_float3(0.419276f, 0.184247f, 0.805721f)) *
            domain_scale;
        const Float3 normalized_surface_normal = safe_normalize(
            surface_normal, make_float3(0.0f, 1.0f, 0.0f));
        const Float3 fallback_axis = ite(
            (normalized_surface_normal.z > -0.999f) &
                (normalized_surface_normal.z < 0.999f),
            make_float3(0.0f, 0.0f, 1.0f),
            make_float3(0.0f, 1.0f, 0.0f));
        const Float3 fallback_tangent = safe_normalize(
            cross(normalized_surface_normal, fallback_axis),
            make_float3(1.0f, 0.0f, 0.0f));
        Float3 transported_normal = safe_normalize(
            surface_tangents.read(strand), fallback_tangent);
        Float3 prior_tangent = normalized_surface_normal;
        Float travelled{0.0f};
        vector<Expr<float3>> displaced;
        displaced.reserve(cvs_per_strand);
        Float3 previous_base = root;
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const Float3 current_base = source.read(first + cv).xyz();
            if (cv != 0u) {
                const Float3 travelled_delta = current_base - previous_base;
                travelled += sqrt(dot(travelled_delta, travelled_delta));
            }
            Float3 next_tangent = prior_tangent;
            if (cv + 1u < cvs_per_strand) {
                const Float3 segment =
                    source.read(first + cv + 1u).xyz() - current_base;
                next_tangent = safe_normalize(segment, prior_tangent);
            }
            const Float3 rotation_axis = cross(prior_tangent, next_tangent);
            const Float rotation_axis_length_squared =
                dot(rotation_axis, rotation_axis);
            const Float3 normalized_axis = safe_normalize(
                rotation_axis, make_float3(1.0f, 0.0f, 0.0f));
            const Float angle = acos(clamp(
                dot(prior_tangent, next_tangent), -1.0f, 1.0f));
            transported_normal = ite(
                rotation_axis_length_squared > 1.0e-20f,
                safe_normalize(
                    rotate_by(transported_normal, normalized_axis, angle),
                    transported_normal),
                transported_normal);
            const Float3 normal = transported_normal;
            const Float3 binormal = cross(normal, next_tangent);
            const Float3 tangent = cross(binormal, normal);
            const auto cv_context = make_context(
                plan, strand, roots, root_runtime,
                original_length, state.y,
                static_cast<float>(cv) /
                    static_cast<float>(cvs_per_strand - 1u));
            const Float magnitude = max(
                lower_classic_runtime_expression(
                    noise.magnitude, cv_context),
                0.0f) * max(
                lower_classic_runtime_expression(
                    noise.magnitude_scale, cv_context),
                0.0f) * mask;
            Float3 output = current_base;
            if (cv != 0u) {
                const Float distance = travelled * effective_frequency;
                const Float3 local = make_float3(
                    xgen_noise(gradients, make_float3(
                        domain.x + distance, domain.y, domain.z)) - 0.5f,
                    xgen_noise(gradients, make_float3(
                        domain.x, domain.y + distance, domain.z)) - 0.5f,
                    xgen_noise(gradients, make_float3(
                        domain.x, domain.y, domain.z + distance)) - 0.5f) *
                    magnitude;
                output = current_base + normal * local.x +
                         binormal * local.y + tangent * local.z;
            }
            displaced.emplace_back(ite(mask > 1.0e-6f, output, current_base));
            prior_tangent = next_tangent;
            previous_base = current_base;
        }
        Float noisy_length{0.0f};
        for (std::uint32_t cv = 1u; cv < cvs_per_strand; ++cv) {
            const Float3 delta = displaced[cv] - displaced[cv - 1u];
            noisy_length += sqrt(dot(delta, delta));
        }
        const Float target_length = original_length * preserve +
                                    noisy_length * (1.0f - preserve);
        const Bool rescale = (preserve > 0.001f) & (noisy_length > 0.0f) &
                             (abs(noisy_length - target_length) >= 0.0001f);
        const Float length_scale = ite(
            rescale, target_length / max(noisy_length, 1.0e-20f), 1.0f);
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const Float3 output = root + (displaced[cv] - root) * length_scale;
            destination.write(first + cv, make_float4(
                output, source.read(first + cv).w));
        }
        states.write(strand, make_float4(
            ite(rescale, target_length, noisy_length),
            state.y, state.z, state.w));
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
                               BufferUInt root_runtime,
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
                plan, strand, roots, root_runtime, c_length, state.y, t);
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
