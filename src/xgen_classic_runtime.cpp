#include "nanoxgen/xgen_classic_runtime.h"

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
         context.v, context.face_seed, context.t},
        scratch);
}

float curve_length(std::span<const PackedCurvePoint> points) {
    float result = 0.0f;
    for (std::size_t index = 1u; index < points.size(); ++index) {
        const Vec3 previous{points[index - 1u].x, points[index - 1u].y,
                            points[index - 1u].z};
        const Vec3 current{points[index].x, points[index].y, points[index].z};
        result += std::sqrt(length_squared(current - previous));
    }
    return result;
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

void cut_and_reparameterize(std::span<PackedCurvePoint> points,
                            float remaining_length,
                            std::vector<float> &cumulative,
                            std::vector<Vec3> &resampled) {
    cumulative.resize(points.size());
    resampled.resize(points.size());
    cumulative[0u] = 0.0f;
    for (std::size_t index = 1u; index < points.size(); ++index) {
        const Vec3 previous{points[index - 1u].x, points[index - 1u].y,
                            points[index - 1u].z};
        const Vec3 current{points[index].x, points[index].y, points[index].z};
        cumulative[index] = cumulative[index - 1u] +
                            std::sqrt(length_squared(current - previous));
    }
    std::size_t segment = 0u;
    for (std::size_t index = 0u; index < points.size(); ++index) {
        const float parameter = static_cast<float>(index) /
                                static_cast<float>(points.size() - 1u);
        const float distance = remaining_length * parameter;
        while (segment + 1u < points.size() &&
               cumulative[segment + 1u] < distance) {
            ++segment;
        }
        if (segment + 1u == points.size()) {
            segment = points.size() - 2u;
        }
        const float interval = cumulative[segment + 1u] - cumulative[segment];
        const float weight = interval > 0.0f
            ? (distance - cumulative[segment]) / interval
            : 0.0f;
        const Vec3 a{points[segment].x, points[segment].y, points[segment].z};
        const Vec3 b{points[segment + 1u].x, points[segment + 1u].y,
                     points[segment + 1u].z};
        resampled[index] = a * (1.0f - weight) + b * weight;
    }
    for (std::size_t index = 0u; index < points.size(); ++index) {
        points[index].x = resampled[index].x;
        points[index].y = resampled[index].y;
        points[index].z = resampled[index].z;
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
                result.cuts.emplace_back(std::move(cut));
            }
        } catch (const std::exception &error) {
            result.fallback_reasons.push_back(
                "CutFXModule " + object_name(object) + ".amount: " +
                error.what());
        }
    }
    if (std::any_of(description.objects.begin(), description.objects.end(),
                    [](const ClassicObject &object) {
                        return object.type == "RandomGenerator";
                    })) {
        result.fallback_reasons.push_back(
            "RandomGenerator: Autodesk root sampling is not implemented");
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
    float radius_scale) {
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
    std::vector<float> cumulative;
    std::vector<Vec3> resampled;
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
        context.id = strand;
        context.u = root.uv.x;
        context.v = root.uv.y;
        context.face_seed = xgen_runtime_face_seed(
            plan.description_id, plan.description_name, root.triangle_index);
        context.c_length = curve_length(points);
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
        for (const ClassicFloatCutModule &cut : plan.cuts) {
            const float amount = evaluate(cut.amount, context);
            if (!std::isfinite(amount)) {
                throw std::runtime_error(
                    "Classic CutFX amount produced a non-finite value");
            }
            const float remaining = std::clamp(
                context.c_length - std::max(amount, 0.0f), 0.0f,
                context.c_length);
            cut_and_reparameterize(points, remaining, cumulative, resampled);
            context.c_length = curve_length(points);
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
}

} // namespace nanoxgen
