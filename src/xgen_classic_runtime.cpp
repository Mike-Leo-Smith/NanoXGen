#include "nanoxgen/xgen_classic_runtime.h"
#include "nanoxgen/seexpr_noise_table.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace nanoxgen {
namespace {

const ClassicObject *unique_object(const ClassicDescription &description,
                                   std::string_view type) {
    const ClassicObject *result = nullptr;
    for (const ClassicObject &object : description.objects) {
        if (object.type != type) { continue; }
        if (result != nullptr) {
            throw std::runtime_error(
                "Classic description has multiple " + std::string{type} +
                " objects");
        }
        result = &object;
    }
    return result;
}

bool object_is_active(const ClassicObject &object) {
    const ClassicAttribute *active = find_classic_attribute(
        object.attributes, "active");
    return active == nullptr ||
           (active->value != "false" && active->value != "False" &&
            active->value != "0");
}

std::string object_name(const ClassicObject &object) {
    const ClassicAttribute *name = find_classic_attribute(
        object.attributes, "name");
    return name == nullptr ? std::string{} : name->value;
}

std::uint32_t parse_uint_attribute(const ClassicAttribute *attribute,
                                   std::uint32_t fallback,
                                   std::string_view label) {
    if (attribute == nullptr || attribute->value.empty()) { return fallback; }
    std::uint32_t result{};
    const char *begin = attribute->value.data();
    const char *end = begin + attribute->value.size();
    const auto converted = std::from_chars(begin, end, result);
    if (converted.ec != std::errc{} || converted.ptr != end) {
        throw std::runtime_error(
            "Classic " + std::string{label} + " must be an unsigned integer");
    }
    return result;
}

bool scalar_attribute_is_zero(
    const ClassicObject &object, std::string_view name) {
    const ClassicAttribute *attribute = find_classic_attribute(
        object.attributes, name);
    if (attribute == nullptr || attribute->value.empty()) { return true; }
    float value{};
    const char *begin = attribute->value.data();
    const char *end = begin + attribute->value.size();
    const auto converted = std::from_chars(begin, end, value);
    return converted.ec == std::errc{} && converted.ptr == end &&
           std::isfinite(value) && value == 0.0f;
}

const ClassicAttribute &required_attribute(
    const ClassicObject &object, std::string_view name) {
    const ClassicAttribute *attribute = find_classic_attribute(
        object.attributes, name);
    if (attribute == nullptr || attribute->value.empty()) {
        throw std::runtime_error("missing " + std::string{name} + " expression");
    }
    return *attribute;
}

ClassicFloatRuntimeExpression compile_expression(
    const ClassicObject &object, const ClassicAttribute &attribute) {
    XgenExpressionCompileOptions options{};
    options.expression_name = attribute.name;
    options.object_type = object.type;
    return {object.type, object_name(object), attribute.name,
            make_xgen_float_expression_program(
                compile_xgen_scalar_expression(attribute.value, options))};
}

void compile_optional(const ClassicObject &object, std::string_view attribute_name,
                      std::optional<ClassicFloatRuntimeExpression> &destination,
                      std::vector<std::string> &fallback_reasons) {
    const ClassicAttribute *attribute = find_classic_attribute(
        object.attributes, attribute_name);
    if (attribute == nullptr || attribute->value.empty()) { return; }
    try {
        destination = compile_expression(object, *attribute);
    } catch (const std::exception &error) {
        fallback_reasons.push_back(
            object.type + "." + std::string{attribute_name} + ": " +
            error.what());
    }
}

bool supported_runtime_input(std::string_view input) {
    // These are the variables whose fast-float binding is unambiguous without
    // retaining Autodesk patch/evaluation state. Patch u/v/face IDs and ri/rf
    // require the native root sampler and therefore remain fallbacks.
    return input == "id" || input == "cLength" || input == "cWidth";
}

bool validate_inputs(const ClassicFloatRuntimeExpression &expression,
                     std::vector<std::string> &fallback_reasons) {
    bool result = true;
    for (const std::string &input : expression.program.inputs) {
        if (!supported_runtime_input(input)) {
            result = false;
            fallback_reasons.push_back(
                expression.object_type + "." + expression.attribute +
                ": runtime variable $" + input + " is not bound");
        }
    }
    return result;
}

void validate_optional_inputs(
    std::optional<ClassicFloatRuntimeExpression> &expression,
    std::vector<std::string> &fallback_reasons) {
    if (expression && !validate_inputs(*expression, fallback_reasons)) {
        expression.reset();
    }
}

float input_value(std::string_view name,
                  const ClassicFloatRuntimeContext &context) {
    if (name == "id") { return static_cast<float>(context.id); }
    if (name == "cLength") { return context.c_length; }
    if (name == "cWidth") { return context.c_width; }
    throw std::runtime_error(
        "Classic runtime variable is not bound: $" + std::string{name});
}

float evaluate_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeContext &context,
    std::span<float> scratch) {
    if (expression.program.inputs.size() > 3u) {
        throw std::runtime_error(
            "Classic runtime expression has too many bound inputs");
    }
    std::array<float, 3u> inputs{};
    for (std::size_t index = 0u; index < expression.program.inputs.size();
         ++index) {
        inputs[index] = input_value(expression.program.inputs[index], context);
    }
    return evaluate_xgen_scalar_expression_float(
        expression.program,
        {std::span{inputs}.first(expression.program.inputs.size()), context.u,
         context.v, context.face_seed, context.t, context.random_prefix,
         context.has_random_prefix},
        scratch);
}

void scale_curve(std::span<PackedCurvePoint> points, float scale) {
    const Vec3 root{points.front().x, points.front().y, points.front().z};
    for (PackedCurvePoint &point : points) {
        const Vec3 value{point.x, point.y, point.z};
        const Vec3 scaled = root + (value - root) * scale;
        point.x = scaled.x;
        point.y = scaled.y;
        point.z = scaled.z;
    }
}

Vec3 xgen_curve_eval(std::span<const Vec3> points, float t) {
    constexpr float epsilon = 1.0e-7f;
    if (t < epsilon) { return points.front(); }
    if (t > 1.0f - epsilon) { return points.back(); }
    const std::uint32_t spans =
        static_cast<std::uint32_t>(points.size() - 1u);
    const float scaled = t * static_cast<float>(spans);
    const std::uint32_t span = static_cast<std::uint32_t>(scaled);
    const float f = scaled - static_cast<float>(span);
    const float f2 = f * f;
    const float f3 = f2 * f;
    const float one_minus_f = 1.0f - f;
    const float b0 = one_minus_f * one_minus_f * one_minus_f;
    const float b1 = 3.0f * f3 - 6.0f * f2 + 4.0f;
    const float b2 = -3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f;
    const float b3 = f3;
    const Vec3 p0 = span == 0u
        ? points[0u] * 2.0f - points[1u]
        : points[span - 1u];
    const Vec3 p1 = points[span];
    const Vec3 p2 = points[span + 1u];
    const Vec3 p3 = span + 2u == points.size()
        ? points.back() * 2.0f - points[points.size() - 2u]
        : points[span + 2u];
    return (p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3) * (1.0f / 6.0f);
}

Vec3 xgen_curve_eval(std::span<const PackedCurvePoint> points, float t) {
    constexpr float epsilon = 1.0e-7f;
    const auto value = [](const PackedCurvePoint &point) {
        return Vec3{point.x, point.y, point.z};
    };
    if (t < epsilon) { return value(points.front()); }
    if (t > 1.0f - epsilon) { return value(points.back()); }
    const std::uint32_t spans =
        static_cast<std::uint32_t>(points.size() - 1u);
    const float scaled = t * static_cast<float>(spans);
    const std::uint32_t span = static_cast<std::uint32_t>(scaled);
    const float f = scaled - static_cast<float>(span);
    const float f2 = f * f;
    const float f3 = f2 * f;
    const float one_minus_f = 1.0f - f;
    const float b0 = one_minus_f * one_minus_f * one_minus_f;
    const float b1 = 3.0f * f3 - 6.0f * f2 + 4.0f;
    const float b2 = -3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f;
    const float b3 = f3;
    const Vec3 p0 = span == 0u
        ? value(points[0u]) * 2.0f - value(points[1u])
        : value(points[span - 1u]);
    const Vec3 p1 = value(points[span]);
    const Vec3 p2 = value(points[span + 1u]);
    const Vec3 p3 = span + 2u == points.size()
        ? value(points.back()) * 2.0f - value(points[points.size() - 2u])
        : value(points[span + 2u]);
    return (p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3) * (1.0f / 6.0f);
}

float curve_spline_length(std::span<const PackedCurvePoint> points) {
    // Match SgCurve::length: subdivide the normalized domain into 2*N + 4
    // intervals, join every interior spline sample, and use the raw endpoint
    // CVs at both ends. cLength-dependent effects are sensitive to this exact
    // fixed approximation (it is deliberately not a converged arc length).
    const std::uint32_t interval_count =
        static_cast<std::uint32_t>(points.size()) * 2u + 4u;
    const float step = 1.0f / static_cast<float>(interval_count);
    Vec3 previous{points.front().x, points.front().y, points.front().z};
    float result = 0.0f;
    for (std::uint32_t sample = 1u; sample < interval_count; ++sample) {
        const Vec3 current = xgen_curve_eval(
            points, step * static_cast<float>(sample));
        result += std::sqrt(length_squared(current - previous));
        previous = current;
    }
    const Vec3 last{points.back().x, points.back().y, points.back().z};
    result += std::sqrt(length_squared(last - previous));
    return result;
}

float curve_spline_length(std::span<const Vec3> points) {
    const std::uint32_t interval_count =
        static_cast<std::uint32_t>(points.size()) * 2u + 4u;
    const float step = 1.0f / static_cast<float>(interval_count);
    Vec3 previous = points.front();
    float result = 0.0f;
    for (std::uint32_t sample = 1u; sample < interval_count; ++sample) {
        const Vec3 current = xgen_curve_eval(
            points, step * static_cast<float>(sample));
        result += std::sqrt(length_squared(current - previous));
        previous = current;
    }
    result += std::sqrt(length_squared(points.back() - previous));
    return result;
}

float curve_polyline_length(std::span<const PackedCurvePoint> points) {
    float result = 0.0f;
    for (std::size_t index = 1u; index < points.size(); ++index) {
        const Vec3 previous{points[index - 1u].x, points[index - 1u].y,
                            points[index - 1u].z};
        const Vec3 current{points[index].x, points[index].y, points[index].z};
        result += std::sqrt(length_squared(current - previous));
    }
    return result;
}

float cut_from_tip_and_rebuild(std::span<PackedCurvePoint> points,
                               float amount,
                               std::vector<Vec3> &source,
                               std::vector<Vec3> &resampled) {
    source.resize(points.size());
    resampled.resize(points.size());
    for (std::size_t index = 0u; index < points.size(); ++index) {
        source[index] = {points[index].x, points[index].y, points[index].z};
    }
    float cut_parameter = 1.0f;
    if (amount >= 1.0e-10f) {
        const float step = 1.0f /
            (2.0f * static_cast<float>(points.size()) + 4.0f);
        float previous_parameter = 1.0f;
        Vec3 previous = source.back();
        float accumulated = 0.0f;
        for (;;) {
            const float parameter = std::max(previous_parameter - step, 0.0f);
            const Vec3 current = xgen_curve_eval(source, parameter);
            const float segment_length =
                std::sqrt(length_squared(previous - current));
            accumulated += segment_length;
            if (accumulated >= amount && segment_length > 0.0f) {
                cut_parameter = parameter +
                    ((accumulated - amount) / segment_length) *
                        (previous_parameter - parameter);
                break;
            }
            if (parameter <= 1.0e-10f) {
                cut_parameter = 0.0f;
                std::fill(resampled.begin(), resampled.end(), source.front());
                break;
            }
            previous_parameter = parameter;
            previous = current;
        }
    }
    if (cut_parameter > 0.0f) {
        const float step = cut_parameter /
            static_cast<float>(points.size() - 1u);
        resampled.front() = source.front();
        for (std::size_t index = 1u; index < points.size(); ++index) {
            resampled[index] = xgen_curve_eval(
                source, step * static_cast<float>(index));
        }
    }
    for (std::size_t index = 0u; index < points.size(); ++index) {
        points[index].x = resampled[index].x;
        points[index].y = resampled[index].y;
        points[index].z = resampled[index].z;
    }
    return cut_parameter;
}

// Match SgCurve::cutToLength followed by SgCurve::cut(..., true). Unlike a
// CutFX amount, the clump guide limits the retained distance measured forward
// from the root. Autodesk searches the same fixed 2*N+4 spline samples used by
// SgCurve::length and then rebuilds the original CV count on [0, cutParam].
void cut_to_length_and_rebuild(std::span<PackedCurvePoint> points,
                               float target_length,
                               std::vector<Vec3> &source,
                               std::vector<Vec3> &resampled) {
    source.resize(points.size());
    resampled.resize(points.size());
    for (std::size_t index = 0u; index < points.size(); ++index) {
        source[index] = {points[index].x, points[index].y, points[index].z};
    }
    if (target_length <= 1.0e-10f) {
        std::fill(resampled.begin(), resampled.end(), source.front());
    } else {
        const std::uint32_t interval_count =
            static_cast<std::uint32_t>(points.size()) * 2u + 4u;
        const float step = 1.0f / static_cast<float>(interval_count);
        float previous_parameter = 0.0f;
        Vec3 previous = source.front();
        float accumulated = 0.0f;
        float cut_parameter = 1.0f;
        for (std::uint32_t sample = 1u; sample <= interval_count; ++sample) {
            const float parameter = sample == interval_count
                ? 1.0f : step * static_cast<float>(sample);
            const Vec3 current = sample == interval_count
                ? source.back() : xgen_curve_eval(source, parameter);
            const float segment_length =
                std::sqrt(length_squared(current - previous));
            accumulated += segment_length;
            if (accumulated >= target_length && segment_length > 0.0f) {
                cut_parameter = parameter -
                    ((accumulated - target_length) / segment_length) *
                        (parameter - previous_parameter);
                break;
            }
            previous_parameter = parameter;
            previous = current;
        }
        const float rebuild_step = cut_parameter /
            static_cast<float>(points.size() - 1u);
        resampled.front() = source.front();
        for (std::size_t index = 1u; index < points.size(); ++index) {
            resampled[index] = xgen_curve_eval(
                source, rebuild_step * static_cast<float>(index));
        }
    }
    for (std::size_t index = 0u; index < points.size(); ++index) {
        points[index].x = resampled[index].x;
        points[index].y = resampled[index].y;
        points[index].z = resampled[index].z;
    }
}

} // namespace

float xgen_classic_noise_float(Vec3 sample) noexcept {
    const float fx = std::floor(sample.x);
    const float fy = std::floor(sample.y);
    const float fz = std::floor(sample.z);
    const std::int32_t ix = static_cast<std::int32_t>(fx);
    const std::int32_t iy = static_cast<std::int32_t>(fy);
    const std::int32_t iz = static_cast<std::int32_t>(fz);
    const Vec3 weights{sample.x - fx, sample.y - fy, sample.z - fz};
    float values[8]{};
    for (std::uint32_t corner = 0u; corner < 8u; ++corner) {
        const std::int32_t ox = static_cast<std::int32_t>(corner & 1u);
        const std::int32_t oy = static_cast<std::int32_t>((corner >> 1u) & 1u);
        const std::int32_t oz = static_cast<std::int32_t>((corner >> 2u) & 1u);
        const Vec3 gradient = detail::kSeExprNoiseGradients[
            noise_hash(ix + ox, iy + oy, iz + oz)];
        values[corner] =
            gradient.x * (weights.x - static_cast<float>(ox)) +
            gradient.y * (weights.y - static_cast<float>(oy)) +
            gradient.z * (weights.z - static_cast<float>(oz));
    }
    const float alphas[3] = {noise_s_curve(weights.x),
                             noise_s_curve(weights.y),
                             noise_s_curve(weights.z)};
    for (std::int32_t dimension = 2; dimension >= 0; --dimension) {
        const std::int32_t count = 1 << dimension;
        for (std::int32_t value = 0; value < count; ++value) {
            const std::int32_t index = value * (1 << (3 - dimension));
            const std::int32_t axis = 2 - dimension;
            const std::int32_t other = index + (1 << axis);
            values[index] = (1.0f - alphas[axis]) * values[index] +
                            alphas[axis] * values[other];
        }
    }
    return 0.5f * values[0] + 0.5f;
}

namespace {

void apply_noise(
    std::span<PackedCurvePoint> points,
    const ClassicFloatNoiseModule &noise,
    ClassicFloatRuntimeContext &context,
    Vec3 surface_normal,
    Vec3 surface_tangent,
    std::span<float> scratch) {
    const auto evaluate = [&](const ClassicFloatRuntimeExpression &expression) {
        return evaluate_runtime_expression(expression, context, scratch);
    };
    const float mask = std::clamp(evaluate(noise.mask), 0.0f, 1.0f);
    if (!(mask > 1.0e-6f)) { return; }
    const float frequency = std::max(evaluate(noise.frequency), 0.0f);
    const float correlation = std::clamp(
        evaluate(noise.correlation) * 0.01f, 0.0f, 1.0f);
    const float preserve = std::clamp(
        evaluate(noise.preserve_length) * 0.01f, 0.0f, 1.0f);
    if (!std::isfinite(frequency) || !std::isfinite(correlation) ||
        !std::isfinite(preserve)) {
        throw std::runtime_error(
            "Classic NoiseFX produced a non-finite parameter");
    }
    const float original_length = curve_polyline_length(points);
    const float effective_frequency = original_length > 0.0f
        ? std::max(0.5f / original_length, frequency)
        : frequency;
    const float decorrelation = 1.0f - correlation;
    const float domain_scale = 100.0f * decorrelation * decorrelation;
    const Vec3 root{points.front().x, points.front().y, points.front().z};
    const Vec3 domain =
        (root * 0.1f + Vec3{0.419276f, 0.184247f, 0.805721f}) * domain_scale;
    if (!(length_squared(surface_tangent) > 1.0e-20f)) {
        surface_tangent = fallback_surface_u(surface_normal);
    } else {
        surface_tangent = normalize(surface_tangent);
    }
    Vec3 previous_base = root;
    Vec3 current_base = root;
    Vec3 next_base{points[1u].x, points[1u].y, points[1u].z};
    // SgCurve::frame transports cU from the surface normal to the first
    // segment, then between successive segment tangents.
    Vec3 prior_tangent = length_squared(surface_normal) > 1.0e-20f
        ? normalize(surface_normal)
        : Vec3{0.0f, 1.0f, 0.0f};
    Vec3 transported_normal = surface_tangent;
    float travelled = 0.0f;
    for (std::uint32_t cv = 0u; cv < points.size(); ++cv) {
        if (cv > 0u) {
            travelled += std::sqrt(length_squared(current_base - previous_base));
        }
        Vec3 next_tangent = prior_tangent;
        if (cv + 1u < points.size()) {
            const Vec3 segment = next_base - current_base;
            if (length_squared(segment) > 1.0e-20f) {
                next_tangent = normalize(segment);
            }
        }
        const Vec3 axis = cross(prior_tangent, next_tangent);
        if (length_squared(axis) > 1.0e-20f) {
            const float angle = std::acos(std::clamp(
                dot(prior_tangent, next_tangent), -1.0f, 1.0f));
            transported_normal = normalize(rotate_by(
                transported_normal, normalize(axis), angle));
        }
        const Vec3 normal = transported_normal;
        const Vec3 binormal = cross(normal, next_tangent);
        const Vec3 tangent = cross(binormal, normal);
        context.t = static_cast<float>(cv) /
                    static_cast<float>(points.size() - 1u);
        const float magnitude = std::max(evaluate(noise.magnitude), 0.0f) *
            std::max(evaluate(noise.magnitude_scale), 0.0f) * mask;
        if (!std::isfinite(magnitude)) {
            throw std::runtime_error(
                "Classic NoiseFX magnitude is non-finite");
        }
        if (cv > 0u) {
            const float distance = travelled * effective_frequency;
            const Vec3 local{
                (xgen_classic_noise_float(
                     {domain.x + distance, domain.y, domain.z}) - 0.5f) *
                    magnitude,
                (xgen_classic_noise_float(
                     {domain.x, domain.y + distance, domain.z}) - 0.5f) *
                    magnitude,
                (xgen_classic_noise_float(
                     {domain.x, domain.y, domain.z + distance}) - 0.5f) *
                    magnitude};
            const Vec3 displaced = current_base + normal * local.x +
                                   binormal * local.y + tangent * local.z;
            points[cv].x = displaced.x;
            points[cv].y = displaced.y;
            points[cv].z = displaced.z;
        }
        prior_tangent = next_tangent;
        previous_base = current_base;
        if (cv + 1u < points.size()) {
            current_base = next_base;
            if (cv + 2u < points.size()) {
                next_base = {points[cv + 2u].x, points[cv + 2u].y,
                             points[cv + 2u].z};
            }
        }
    }
    if (preserve > 0.001f) {
        const float noisy_length = curve_polyline_length(points);
        if (noisy_length > 0.0f) {
            const float target = original_length * preserve +
                                 noisy_length * (1.0f - preserve);
            if (std::abs(noisy_length - target) >= 0.0001f) {
                scale_curve(points, target / noisy_length);
            }
        }
    }
}

} // namespace

ClassicFloatRuntimePlan compile_xgen_classic_float_runtime_plan(
    const ClassicDescription &description) {
    ClassicFloatRuntimePlan result{};
    result.description_name = description.name;
    result.description_id = parse_uint_attribute(
        find_classic_attribute(description.attributes, "descriptionId"), 0u,
        "descriptionId");
    const ClassicObject *primitive = unique_object(description, "SplinePrimitive");
    if (primitive == nullptr) {
        throw std::runtime_error(
            "Classic description has no SplinePrimitive object");
    }
    result.fx_cv_count = parse_uint_attribute(
        find_classic_attribute(primitive->attributes, "fxCVCount"), 0u,
        "fxCVCount");
    compile_optional(*primitive, "length", result.length,
                     result.fallback_reasons);
    compile_optional(*primitive, "width", result.width,
                     result.fallback_reasons);
    compile_optional(*primitive, "taper", result.taper,
                     result.fallback_reasons);
    compile_optional(*primitive, "taperStart", result.taper_start,
                     result.fallback_reasons);
    compile_optional(*primitive, "widthRamp", result.width_ramp,
                     result.fallback_reasons);
    validate_optional_inputs(result.length, result.fallback_reasons);
    validate_optional_inputs(result.width, result.fallback_reasons);
    validate_optional_inputs(result.taper, result.fallback_reasons);
    validate_optional_inputs(result.taper_start, result.fallback_reasons);
    validate_optional_inputs(result.width_ramp, result.fallback_reasons);

    for (const ClassicObject &object : description.objects) {
        if (!object.type.ends_with("FXModule") || !object_is_active(object)) {
            continue;
        }
        if (object.type == "NoiseFXModule") {
            const std::uint32_t mode = parse_uint_attribute(
                find_classic_attribute(object.attributes, "mode"), 0u,
                "NoiseFXModule mode");
            if (mode != 0u) {
                result.fallback_reasons.push_back(
                    "NoiseFXModule " + object_name(object) +
                    ": only mode 0 is implemented");
                continue;
            }
            const auto attribute = [&](std::string_view name) {
                const ClassicAttribute *value = find_classic_attribute(
                    object.attributes, name);
                if (value == nullptr || value->value.empty()) {
                    throw std::runtime_error(
                        "missing " + std::string{name} + " expression");
                }
                return value;
            };
            try {
                ClassicFloatNoiseModule noise{
                    compile_expression(object, *attribute("mask")),
                    compile_expression(object, *attribute("magnitude")),
                    compile_expression(object, *attribute("magnitudeScale")),
                    compile_expression(object, *attribute("frequency")),
                    compile_expression(object, *attribute("correlation")),
                    compile_expression(object, *attribute("preserveLength")),
                    mode};
                bool valid = true;
                valid &= validate_inputs(noise.mask, result.fallback_reasons);
                valid &= validate_inputs(
                    noise.magnitude, result.fallback_reasons);
                valid &= validate_inputs(
                    noise.magnitude_scale, result.fallback_reasons);
                valid &= validate_inputs(
                    noise.frequency, result.fallback_reasons);
                valid &= validate_inputs(
                    noise.correlation, result.fallback_reasons);
                valid &= validate_inputs(
                    noise.preserve_length, result.fallback_reasons);
                if (valid) {
                    const std::uint32_t index = static_cast<std::uint32_t>(
                        result.noises.size());
                    result.noises.emplace_back(std::move(noise));
                    result.effects.push_back(
                        {ClassicFloatEffectType::Noise, index});
                }
            } catch (const std::exception &error) {
                result.fallback_reasons.push_back(
                    "NoiseFXModule " + object_name(object) + ": " +
                    error.what());
            }
            continue;
        }
        if (object.type == "ClumpingFXModule") {
            const std::string name = object_name(object);
            const std::string_view unsupported_zero_attributes[]{
                "clumpVariance", "cut", "copy", "copyVariance", "curl",
                "offset", "flatness", "frame", "noise"};
            std::string unsupported;
            for (const std::string_view attribute :
                 unsupported_zero_attributes) {
                if (!scalar_attribute_is_zero(object, attribute)) {
                    if (!unsupported.empty()) { unsupported += ", "; }
                    unsupported += attribute;
                }
            }
            const std::uint32_t use_control_maps = parse_uint_attribute(
                find_classic_attribute(object.attributes, "useControlMaps"),
                0u, "ClumpingFXModule useControlMaps");
            if (use_control_maps != 0u) {
                if (!unsupported.empty()) { unsupported += ", "; }
                unsupported += "useControlMaps";
            }
            const ClassicAttribute *volumize = find_classic_attribute(
                object.attributes, "clumpVolumize");
            if (volumize && volumize->value != "false" &&
                volumize->value != "False" && volumize->value != "0") {
                if (!unsupported.empty()) { unsupported += ", "; }
                unsupported += "clumpVolumize";
            }
            if (!unsupported.empty()) {
                result.fallback_reasons.push_back(
                    "ClumpingFXModule " + name +
                    ": unsupported authored controls: " + unsupported);
                continue;
            }
            try {
                ClassicFloatClumpModule clump{
                    name,
                    compile_expression(
                        object, required_attribute(object, "mask")),
                    compile_expression(
                        object, required_attribute(object, "clump")),
                    compile_expression(
                        object, required_attribute(object, "clumpScale"))};
                bool valid = true;
                valid &= validate_inputs(clump.mask, result.fallback_reasons);
                valid &= validate_inputs(clump.clump, result.fallback_reasons);
                valid &= validate_inputs(
                    clump.clump_scale, result.fallback_reasons);
                if (valid) {
                    const std::uint32_t index = static_cast<std::uint32_t>(
                        result.clumps.size());
                    result.clumps.emplace_back(std::move(clump));
                    result.effects.push_back(
                        {ClassicFloatEffectType::Clump, index});
                }
            } catch (const std::exception &error) {
                result.fallback_reasons.push_back(
                    "ClumpingFXModule " + name + ": " + error.what());
            }
            continue;
        }
        if (object.type != "CutFXModule") {
            result.fallback_reasons.push_back(
                object.type + " " + object_name(object) +
                ": authored FX evaluation is not implemented");
            continue;
        }
        const ClassicAttribute *amount = find_classic_attribute(
            object.attributes, "amount");
        if (amount == nullptr || amount->value.empty()) {
            result.fallback_reasons.push_back(
                "CutFXModule " + object_name(object) +
                ": missing amount expression");
            continue;
        }
        const std::uint32_t rebuild_type = parse_uint_attribute(
            find_classic_attribute(object.attributes, "rebuildType"), 0u,
            "CutFXModule rebuildType");
        if (rebuild_type != 1u) {
            result.fallback_reasons.push_back(
                "CutFXModule " + object_name(object) +
                ": only rebuildType 1 (reparameterize) is implemented");
            continue;
        }
        try {
            ClassicFloatCutModule cut{compile_expression(object, *amount),
                                      rebuild_type};
            if (validate_inputs(cut.amount, result.fallback_reasons)) {
                const std::uint32_t index = static_cast<std::uint32_t>(
                    result.cuts.size());
                result.cuts.emplace_back(std::move(cut));
                result.effects.push_back(
                    {ClassicFloatEffectType::Cut, index});
            }
        } catch (const std::exception &error) {
            result.fallback_reasons.push_back(
                "CutFXModule " + object_name(object) + ".amount: " +
                error.what());
        }
    }
    return result;
}

float evaluate_xgen_classic_float_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeContext &context) {
    std::vector<float> scratch(expression.program.instructions.size());
    return evaluate_runtime_expression(expression, context, scratch);
}

void apply_xgen_classic_float_runtime_plan_cpu(
    PackedGeneratedCurves &curves,
    const ClassicFloatRuntimePlan &plan,
    float radius_scale,
    std::span<const Vec3> surface_tangents,
    std::span<const std::uint32_t> random_prefixes,
    std::span<const std::uint32_t> primitive_ids,
    std::span<const ClassicClumpRuntimeData> clump_data) {
    if (curves.strand_count == 0u || curves.cvs_per_strand < 2u ||
        curves.point_counts.size() != curves.strand_count ||
        curves.roots.size() != curves.strand_count ||
        curves.points.size() != static_cast<std::size_t>(curves.strand_count) *
                                    curves.cvs_per_strand) {
        throw std::invalid_argument(
            "Classic runtime needs a valid fixed-CV packed curve set");
    }
    if (!std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument(
            "Classic runtime radius scale must be finite and non-negative");
    }
    if (!plan.noises.empty() &&
        surface_tangents.size() != curves.strand_count) {
        throw std::invalid_argument(
            "Classic NoiseFX needs one surface tangent per strand");
    }
    bool needs_random_prefix = false;
    const auto expression_needs_prefix = [&](
        const std::optional<ClassicFloatRuntimeExpression> &expression) {
        return expression && expression->program.random_call_count != 0u;
    };
    needs_random_prefix |= expression_needs_prefix(plan.length) ||
        expression_needs_prefix(plan.width) ||
        expression_needs_prefix(plan.taper) ||
        expression_needs_prefix(plan.taper_start) ||
        expression_needs_prefix(plan.width_ramp);
    for (const ClassicFloatCutModule &cut : plan.cuts) {
        needs_random_prefix |= cut.amount.program.random_call_count != 0u;
    }
    for (const ClassicFloatNoiseModule &noise : plan.noises) {
        needs_random_prefix |= noise.mask.program.random_call_count != 0u ||
            noise.magnitude.program.random_call_count != 0u ||
            noise.magnitude_scale.program.random_call_count != 0u ||
            noise.frequency.program.random_call_count != 0u ||
            noise.correlation.program.random_call_count != 0u ||
            noise.preserve_length.program.random_call_count != 0u;
    }
    for (const ClassicFloatClumpModule &clump : plan.clumps) {
        needs_random_prefix |= clump.mask.program.random_call_count != 0u ||
            clump.clump.program.random_call_count != 0u ||
            clump.clump_scale.program.random_call_count != 0u;
    }
    if (needs_random_prefix &&
        random_prefixes.size() != curves.strand_count) {
        throw std::invalid_argument(
            "Classic runtime needs one SeExpr prefix per strand");
    }
    if (!primitive_ids.empty() &&
        primitive_ids.size() != curves.strand_count) {
        throw std::invalid_argument(
            "Classic runtime needs one primitive ID per strand");
    }
    if (clump_data.size() != plan.clumps.size()) {
        throw std::invalid_argument(
            "Classic runtime needs one geometry binding per ClumpingFX module");
    }
    for (std::size_t module = 0u; module < clump_data.size(); ++module) {
        const ClassicClumpRuntimeData &data = clump_data[module];
        if (data.module_name != plan.clumps[module].name ||
            data.cvs_per_guide != curves.cvs_per_strand ||
            data.strand_guide_indices.size() != curves.strand_count ||
            data.guide_axes.size() % curves.cvs_per_strand != 0u) {
            throw std::invalid_argument(
                "Classic ClumpingFX geometry binding is inconsistent");
        }
    }
    std::vector<std::vector<float>> clump_guide_lengths(clump_data.size());
    for (std::size_t module = 0u; module < clump_data.size(); ++module) {
        const ClassicClumpRuntimeData &data = clump_data[module];
        const std::size_t guide_count =
            data.guide_axes.size() / curves.cvs_per_strand;
        std::vector<float> &lengths = clump_guide_lengths[module];
        lengths.resize(guide_count);
        for (std::size_t guide = 0u; guide < guide_count; ++guide) {
            lengths[guide] = curve_spline_length(std::span<const Vec3>{
                data.guide_axes.data() + guide * curves.cvs_per_strand,
                curves.cvs_per_strand});
        }
    }
    std::vector<Vec3> cut_source;
    std::vector<Vec3> resampled;
    std::vector<std::uint8_t> keep(curves.strand_count, 1u);
    std::size_t scratch_size = 0u;
    const auto include_scratch = [&](
        const std::optional<ClassicFloatRuntimeExpression> &expression) {
        if (expression) {
            scratch_size = std::max(
                scratch_size, expression->program.instructions.size());
        }
    };
    include_scratch(plan.length);
    include_scratch(plan.width);
    include_scratch(plan.taper);
    include_scratch(plan.taper_start);
    include_scratch(plan.width_ramp);
    for (const ClassicFloatCutModule &cut : plan.cuts) {
        scratch_size = std::max(
            scratch_size, cut.amount.program.instructions.size());
    }
    for (const ClassicFloatNoiseModule &noise : plan.noises) {
        const ClassicFloatRuntimeExpression *expressions[] = {
            &noise.mask, &noise.magnitude, &noise.magnitude_scale,
            &noise.frequency, &noise.correlation, &noise.preserve_length};
        for (const ClassicFloatRuntimeExpression *expression : expressions) {
            scratch_size = std::max(
                scratch_size, expression->program.instructions.size());
        }
    }
    for (const ClassicFloatClumpModule &clump : plan.clumps) {
        const ClassicFloatRuntimeExpression *expressions[] = {
            &clump.mask, &clump.clump, &clump.clump_scale};
        for (const ClassicFloatRuntimeExpression *expression : expressions) {
            scratch_size = std::max(
                scratch_size, expression->program.instructions.size());
        }
    }
    std::vector<float> scratch(scratch_size);
    const auto evaluate = [&](const ClassicFloatRuntimeExpression &expression,
                              const ClassicFloatRuntimeContext &context) {
        return evaluate_runtime_expression(expression, context, scratch);
    };
    for (std::uint32_t strand = 0u; strand < curves.strand_count; ++strand) {
        if (curves.point_counts[strand] != curves.cvs_per_strand) {
            throw std::invalid_argument(
                "Classic runtime requires fixed point counts");
        }
        std::span<PackedCurvePoint> points{curves.points.data() +
                static_cast<std::size_t>(strand) * curves.cvs_per_strand,
            curves.cvs_per_strand};
        const RootSample &root = curves.roots[strand];
        ClassicFloatRuntimeContext context{};
        context.id = primitive_ids.empty() ? strand : primitive_ids[strand];
        context.u = root.uv.x;
        context.v = root.uv.y;
        context.face_seed = xgen_runtime_face_seed(
            plan.description_id, plan.description_name,
            root.surface_face_id == kInvalidIndex
                ? root.triangle_index
                : root.surface_face_id);
        context.random_prefix = random_prefixes.empty()
            ? 0u : random_prefixes[strand];
        context.has_random_prefix = !random_prefixes.empty();
        context.c_length = curve_spline_length(points);
        if (plan.length) {
            const float length_scale = evaluate(*plan.length, context);
            if (!std::isfinite(length_scale) || length_scale < 0.0f) {
                throw std::runtime_error(
                    "Classic length expression produced a negative value");
            }
            scale_curve(points, length_scale);
            context.c_length *= length_scale;
        }
        context.c_width = plan.width
            ? evaluate(*plan.width, context)
            : (radius_scale > 0.0f
                  ? 2.0f * points.front().radius / radius_scale
                  : 0.0f);
        if (!std::isfinite(context.c_width) || context.c_width < 0.0f) {
            throw std::runtime_error(
                "Classic width expression produced a negative value");
        }
        for (const ClassicFloatEffect effect : plan.effects) {
            if (effect.type == ClassicFloatEffectType::Clump) {
                if (effect.module_index >= plan.clumps.size()) {
                    throw std::invalid_argument(
                        "Classic ClumpingFX operation index is invalid");
                }
                const ClassicFloatClumpModule &clump =
                    plan.clumps[effect.module_index];
                const ClassicClumpRuntimeData &data =
                    clump_data[effect.module_index];
                const std::uint32_t guide_index =
                    data.strand_guide_indices[strand];
                const std::uint32_t guide_count = static_cast<std::uint32_t>(
                    data.guide_axes.size() / curves.cvs_per_strand);
                if (guide_index != kInvalidIndex) {
                    if (guide_index >= guide_count) {
                        throw std::invalid_argument(
                            "Classic ClumpingFX guide index is invalid");
                    }
                    const std::span<const Vec3> axis{
                        data.guide_axes.data() +
                            static_cast<std::size_t>(guide_index) *
                                curves.cvs_per_strand,
                        curves.cvs_per_strand};
                    context.t = 0.0f;
                    const float mask = std::clamp(
                        evaluate(clump.mask, context), 0.0f, 1.0f);
                    if (mask <= 0.0f) { continue; }
                    const float guide_length =
                        clump_guide_lengths[effect.module_index][guide_index];
                    // Clumping always rebuilds an affected curve, even when
                    // the guide is longer and the retained parameter is 1.
                    // That spline-to-CV resampling is observable in Autodesk
                    // output and is not equivalent to leaving the CVs alone.
                    cut_to_length_and_rebuild(
                        points, guide_length, cut_source, resampled);
                    context.c_length = curve_spline_length(points);
                    const float amount = std::clamp(
                        evaluate(clump.clump, context), 0.0f, 1.0f);
                    for (std::uint32_t cv = 1u;
                         cv < curves.cvs_per_strand; ++cv) {
                        context.t = static_cast<float>(cv) /
                            static_cast<float>(curves.cvs_per_strand - 1u);
                        const float scale = evaluate(clump.clump_scale, context);
                        if (!std::isfinite(scale)) {
                            throw std::runtime_error(
                                "Classic ClumpingFX scale is non-finite");
                        }
                        const Vec3 goal = xgen_curve_eval(axis, context.t);
                        const Vec3 current{
                            points[cv].x, points[cv].y, points[cv].z};
                        const Vec3 output = current + (goal - current) *
                            (mask * amount * (1.0f - 2.0f * scale));
                        points[cv].x = output.x;
                        points[cv].y = output.y;
                        points[cv].z = output.z;
                    }
                    context.c_length = curve_spline_length(points);
                }
            } else if (effect.type == ClassicFloatEffectType::Noise) {
                if (effect.module_index >= plan.noises.size()) {
                    throw std::invalid_argument(
                        "Classic NoiseFX operation index is invalid");
                }
                apply_noise(points, plan.noises[effect.module_index], context,
                            root.normal, surface_tangents[strand], scratch);
                context.c_length = curve_spline_length(points);
            } else if (effect.type == ClassicFloatEffectType::Cut) {
                if (effect.module_index >= plan.cuts.size()) {
                    throw std::invalid_argument(
                        "Classic CutFX operation index is invalid");
                }
                const ClassicFloatCutModule &cut =
                    plan.cuts[effect.module_index];
                const float amount = evaluate(cut.amount, context);
                if (!std::isfinite(amount)) {
                    throw std::runtime_error(
                        "Classic CutFX amount produced a non-finite value");
                }
                const float cut_parameter = cut_from_tip_and_rebuild(
                    points, std::max(amount, 0.0f), cut_source, resampled);
                if (cut_parameter < 1.0e-4f) { keep[strand] = 0u; }
                context.c_length = curve_spline_length(points);
            } else {
                throw std::invalid_argument(
                    "Classic runtime effect type is invalid");
            }
        }
        const float taper = plan.taper
            ? evaluate(*plan.taper, context)
            : 0.0f;
        const float taper_start = plan.taper_start
            ? evaluate(*plan.taper_start, context)
            : 0.0f;
        if (!std::isfinite(taper) || !std::isfinite(taper_start)) {
            throw std::runtime_error(
                "Classic taper expression produced a non-finite value");
        }
        const bool apply_width_profile = plan.width || plan.taper ||
                                         plan.taper_start || plan.width_ramp;
        for (std::uint32_t cv = 0u;
             apply_width_profile && cv < curves.cvs_per_strand; ++cv) {
            context.t = static_cast<float>(cv) /
                        static_cast<float>(curves.cvs_per_strand - 1u);
            float scale = 1.0f;
            if (context.t > taper_start && taper_start < 1.0f) {
                scale *= 1.0f - taper *
                    ((context.t - taper_start) / (1.0f - taper_start));
            }
            if (plan.width_ramp) {
                scale *= evaluate(*plan.width_ramp, context);
            }
            const float diameter = context.c_width * scale;
            if (!std::isfinite(diameter) || diameter < 0.0f) {
                throw std::runtime_error(
                    "Classic width profile produced a negative value");
            }
            points[cv].radius = 0.5f * diameter * radius_scale;
        }
    }
    std::uint32_t write = 0u;
    for (std::uint32_t read = 0u; read < curves.strand_count; ++read) {
        if (keep[read] == 0u) { continue; }
        if (write != read) {
            for (std::uint32_t cv = 0u; cv < curves.cvs_per_strand; ++cv) {
                curves.points[static_cast<std::size_t>(write) *
                                  curves.cvs_per_strand + cv] =
                    curves.points[static_cast<std::size_t>(read) *
                                      curves.cvs_per_strand + cv];
            }
            curves.point_counts[write] = curves.point_counts[read];
            curves.roots[write] = curves.roots[read];
            if (!curves.root_uvs.empty()) {
                curves.root_uvs[write] = curves.root_uvs[read];
            }
        }
        ++write;
    }
    if (write != curves.strand_count) {
        curves.strand_count = write;
        curves.point_counts.resize(write);
        curves.roots.resize(write);
        if (!curves.root_uvs.empty()) { curves.root_uvs.resize(write); }
        curves.points.resize(
            static_cast<std::size_t>(write) * curves.cvs_per_strand);
    }
}

void add_xgen_classic_renderer_endpoints(PackedGeneratedCurves &curves) {
    if (curves.strand_count == 0u || curves.cvs_per_strand < 2u ||
        curves.point_counts.size() != curves.strand_count ||
        curves.points.size() != static_cast<std::size_t>(curves.strand_count) *
                                    curves.cvs_per_strand) {
        throw std::invalid_argument(
            "Classic renderer endpoints need valid fixed-CV curves");
    }
    const std::uint32_t source_count = curves.cvs_per_strand;
    const std::uint32_t output_count = source_count + 2u;
    std::vector<PackedCurvePoint> output(
        static_cast<std::size_t>(curves.strand_count) * output_count);
    for (std::uint32_t strand = 0u; strand < curves.strand_count; ++strand) {
        const PackedCurvePoint *source = curves.points.data() +
            static_cast<std::size_t>(strand) * source_count;
        PackedCurvePoint *destination = output.data() +
            static_cast<std::size_t>(strand) * output_count;
        std::copy_n(source, source_count, destination + 1u);
        destination[0u] = {
            2.0f * source[0u].x - source[1u].x,
            2.0f * source[0u].y - source[1u].y,
            2.0f * source[0u].z - source[1u].z,
            source[0u].radius};
        destination[output_count - 1u] = {
            2.0f * source[source_count - 1u].x -
                source[source_count - 2u].x,
            2.0f * source[source_count - 1u].y -
                source[source_count - 2u].y,
            2.0f * source[source_count - 1u].z -
                source[source_count - 2u].z,
            source[source_count - 1u].radius};
    }
    curves.cvs_per_strand = output_count;
    std::fill(curves.point_counts.begin(), curves.point_counts.end(), output_count);
    curves.points = std::move(output);
}

} // namespace nanoxgen
