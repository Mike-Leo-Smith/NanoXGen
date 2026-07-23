#include "nanoxgen/xgen_classic_runtime.h"
#include "nanoxgen/seexpr_noise_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
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

bool identifier_character(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

std::optional<float> constant_local_assignment(
    std::string_view source, std::string_view name, std::size_t before) {
    const std::string token = "$" + std::string{name};
    std::optional<float> result;
    for (std::size_t index = 0u; index < before;) {
        index = source.find(token, index);
        if (index == std::string_view::npos || index >= before) { break; }
        std::size_t cursor = index + token.size();
        if (cursor < source.size() && identifier_character(source[cursor])) {
            index = cursor;
            continue;
        }
        while (cursor < before &&
               (source[cursor] == ' ' || source[cursor] == '\t' ||
                source[cursor] == '\r' || source[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= before || source[cursor] != '=' ||
            (cursor + 1u < before && source[cursor + 1u] == '=')) {
            index = cursor;
            continue;
        }
        ++cursor;
        while (cursor < before &&
               (source[cursor] == ' ' || source[cursor] == '\t' ||
                source[cursor] == '\r' || source[cursor] == '\n')) {
            ++cursor;
        }
        float sign = 1.0f;
        if (cursor < before &&
            (source[cursor] == '+' || source[cursor] == '-')) {
            if (source[cursor++] == '-') { sign = -1.0f; }
        }
        float value{};
        const auto converted = std::from_chars(
            source.data() + cursor, source.data() + before, value);
        if (converted.ec == std::errc{} && converted.ptr != source.data() + cursor &&
            std::isfinite(value)) {
            result = sign * value;
        }
        index = cursor;
    }
    return result;
}

ClassicFloatRuntimeExpression compile_expression(
    const ClassicObject &object, const ClassicAttribute &attribute,
    std::vector<std::string> &ptex_paths,
    std::span<const ClassicAttribute> palette_attributes,
    std::vector<ClassicFloatCustomInput> &custom_inputs,
    std::vector<ClassicFloatPrefNoiseInput> &pref_noise_inputs) {
    std::string source;
    source.reserve(attribute.value.size());
    bool quoted = false;
    char quote{};
    for (std::size_t index = 0u; index < attribute.value.size();) {
        char c = attribute.value[index];
        if (!quoted && c == '\\' && index + 1u < attribute.value.size() &&
            attribute.value[index + 1u] == 'n') {
            source.push_back('\n');
            index += 2u;
            continue;
        }
        if (!quoted && c == '#') {
            while (index < attribute.value.size() &&
                   attribute.value[index] != '\n') {
                if (attribute.value[index] == '\\' &&
                    index + 1u < attribute.value.size() &&
                    attribute.value[index + 1u] == 'n') {
                    index += 2u;
                    source.push_back('\n');
                    break;
                }
                ++index;
            }
            continue;
        }
        source.push_back(c);
        if (quoted) {
            if (c == quote &&
                (index == 0u || attribute.value[index - 1u] != '\\')) {
                quoted = false;
            }
        } else if (c == '\'' || c == '"') {
            quoted = true;
            quote = c;
        }
        ++index;
    }

    std::string rewritten;
    rewritten.reserve(source.size());
    for (std::size_t index = 0u; index < source.size();) {
        if (source.compare(index, 3u, "map") != 0 ||
            (index != 0u &&
             ((source[index - 1u] >= 'a' && source[index - 1u] <= 'z') ||
              (source[index - 1u] >= 'A' && source[index - 1u] <= 'Z') ||
              (source[index - 1u] >= '0' && source[index - 1u] <= '9') ||
              source[index - 1u] == '_'))) {
            rewritten.push_back(source[index++]);
            continue;
        }
        std::size_t cursor = index + 3u;
        while (cursor < source.size() &&
               (source[cursor] == ' ' || source[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor++] != '(') {
            rewritten.push_back(source[index++]);
            continue;
        }
        while (cursor < source.size() &&
               (source[cursor] == ' ' || source[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= source.size() ||
            (source[cursor] != '\'' && source[cursor] != '"')) {
            throw std::runtime_error("map() path must be a quoted string");
        }
        const char map_quote = source[cursor++];
        const std::size_t path_begin = cursor;
        while (cursor < source.size() && source[cursor] != map_quote) {
            ++cursor;
        }
        if (cursor >= source.size()) {
            throw std::runtime_error("unterminated map() path");
        }
        const std::string path = source.substr(path_begin, cursor - path_begin);
        ++cursor;
        while (cursor < source.size() &&
               (source[cursor] == ' ' || source[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor++] != ')') {
            throw std::runtime_error("map() is missing a closing parenthesis");
        }
        const std::size_t map_index = ptex_paths.size();
        if (map_index >= 61u) {
            throw std::runtime_error("runtime PTEX input limit exceeded");
        }
        ptex_paths.push_back(path);
        rewritten += "$__nxg_map_" + std::to_string(map_index);
        index = cursor;
    }
    // The scalar IR deliberately has no vector values. Bind Classic's
    // reference-global position noise as a root input, retaining the local
    // scalar frequency in the authored expression. More complex vector noise
    // forms remain checked fallbacks.
    for (std::size_t index = 0u;;) {
        index = rewritten.find("noise", index);
        if (index == std::string::npos) { break; }
        if ((index != 0u && identifier_character(rewritten[index - 1u])) ||
            (index + 5u < rewritten.size() &&
             identifier_character(rewritten[index + 5u]))) {
            index += 5u;
            continue;
        }
        std::size_t cursor = index + 5u;
        while (cursor < rewritten.size() &&
               (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= rewritten.size() || rewritten[cursor++] != '(') {
            index += 5u;
            continue;
        }
        while (cursor < rewritten.size() &&
               (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
            ++cursor;
        }
        constexpr std::string_view prefg{"$Prefg"};
        if (rewritten.compare(cursor, prefg.size(), prefg) != 0 ||
            (cursor + prefg.size() < rewritten.size() &&
             identifier_character(rewritten[cursor + prefg.size()]))) {
            index += 5u;
            continue;
        }
        cursor += prefg.size();
        while (cursor < rewritten.size() &&
               (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= rewritten.size() || rewritten[cursor++] != '*') {
            index += 5u;
            continue;
        }
        while (cursor < rewritten.size() &&
               (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
            ++cursor;
        }
        std::optional<float> frequency;
        if (cursor < rewritten.size() && rewritten[cursor] == '$') {
            const std::size_t name_begin = ++cursor;
            while (cursor < rewritten.size() &&
                   identifier_character(rewritten[cursor])) {
                ++cursor;
            }
            if (cursor != name_begin) {
                frequency = constant_local_assignment(
                    rewritten,
                    std::string_view{rewritten}.substr(
                        name_begin, cursor - name_begin),
                    index);
            }
        } else {
            float sign = 1.0f;
            if (cursor < rewritten.size() &&
                (rewritten[cursor] == '+' || rewritten[cursor] == '-')) {
                if (rewritten[cursor++] == '-') { sign = -1.0f; }
            }
            float value{};
            const auto converted = std::from_chars(
                rewritten.data() + cursor,
                rewritten.data() + rewritten.size(), value);
            if (converted.ec == std::errc{} &&
                converted.ptr != rewritten.data() + cursor &&
                std::isfinite(value)) {
                frequency = sign * value;
                cursor = static_cast<std::size_t>(
                    converted.ptr - rewritten.data());
            }
        }
        while (cursor < rewritten.size() &&
               (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
            ++cursor;
        }
        if (!frequency || cursor >= rewritten.size() ||
            rewritten[cursor++] != ')') {
            index += 5u;
            continue;
        }
        auto found = std::find_if(
            pref_noise_inputs.begin(), pref_noise_inputs.end(),
            [frequency](const ClassicFloatPrefNoiseInput &input) {
                return std::bit_cast<std::uint32_t>(input.frequency) ==
                       std::bit_cast<std::uint32_t>(*frequency);
            });
        std::size_t noise_index{};
        if (found == pref_noise_inputs.end()) {
            if (ptex_paths.size() + custom_inputs.size() +
                    pref_noise_inputs.size() >= 61u) {
                throw std::runtime_error(
                    "runtime external input limit exceeded");
            }
            noise_index = pref_noise_inputs.size();
            pref_noise_inputs.push_back({*frequency});
        } else {
            noise_index = static_cast<std::size_t>(
                found - pref_noise_inputs.begin());
        }
        const std::string input =
            "$__nxg_pref_noise_" + std::to_string(noise_index);
        rewritten.replace(index, cursor - index, input);
        index += input.size();
    }
    constexpr std::string_view custom_prefix{"custom_float_"};
    for (const ClassicAttribute &custom_attribute : palette_attributes) {
        if (!custom_attribute.name.starts_with(custom_prefix) ||
            custom_attribute.name.size() == custom_prefix.size()) {
            continue;
        }
        const std::string name = custom_attribute.name.substr(
            custom_prefix.size());
        for (std::size_t index = 0u;;) {
            index = rewritten.find(name, index);
            if (index == std::string::npos) { break; }
            if ((index != 0u && identifier_character(rewritten[index - 1u])) ||
                (index + name.size() < rewritten.size() &&
                 identifier_character(rewritten[index + name.size()]))) {
                index += name.size();
                continue;
            }
            std::size_t cursor = index + name.size();
            while (cursor < rewritten.size() &&
                   (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor >= rewritten.size() || rewritten[cursor++] != '(') {
                index += name.size();
                continue;
            }
            while (cursor < rewritten.size() &&
                   (rewritten[cursor] == ' ' || rewritten[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor >= rewritten.size() || rewritten[cursor++] != ')') {
                throw std::runtime_error(
                    "palette custom function " + name +
                    " must be called without arguments");
            }
            auto found = std::find_if(
                custom_inputs.begin(), custom_inputs.end(),
                [&name](const ClassicFloatCustomInput &input) {
                    return input.name == name;
                });
            std::size_t custom_index{};
            if (found == custom_inputs.end()) {
                if (ptex_paths.size() + custom_inputs.size() +
                        pref_noise_inputs.size() >= 61u) {
                    throw std::runtime_error(
                        "runtime external input limit exceeded");
                }
                XgenExpressionCompileOptions custom_options{};
                custom_options.expression_name = name;
                custom_options.object_type = "Palette";
                XgenFloatExpressionProgram program =
                    make_xgen_float_expression_program(
                        compile_xgen_scalar_expression(
                            custom_attribute.value, custom_options));
                for (const std::string &input : program.inputs) {
                    if (input != "id") {
                        throw std::runtime_error(
                            "palette custom function " + name +
                            " uses unsupported variable $" + input);
                    }
                }
                custom_index = custom_inputs.size();
                custom_inputs.push_back({name, std::move(program)});
            } else {
                custom_index = static_cast<std::size_t>(
                    found - custom_inputs.begin());
            }
            const std::string input =
                "$__nxg_custom_" + std::to_string(custom_index);
            rewritten.replace(index, cursor - index, input);
            index += input.size();
        }
    }
    XgenExpressionCompileOptions options{};
    options.expression_name = attribute.name;
    options.object_type = object.type;
    return {object.type, object_name(object), attribute.name,
            make_xgen_float_expression_program(
                compile_xgen_scalar_expression(rewritten, options))};
}

void compile_optional(const ClassicObject &object, std::string_view attribute_name,
                      std::optional<ClassicFloatRuntimeExpression> &destination,
                      std::vector<std::string> &fallback_reasons,
                      std::vector<std::string> &ptex_paths,
                      std::span<const ClassicAttribute> palette_attributes,
                      std::vector<ClassicFloatCustomInput> &custom_inputs,
                      std::vector<ClassicFloatPrefNoiseInput> &pref_noise_inputs) {
    const ClassicAttribute *attribute = find_classic_attribute(
        object.attributes, attribute_name);
    if (attribute == nullptr || attribute->value.empty()) { return; }
    const std::size_t map_count = ptex_paths.size();
    const std::size_t custom_count = custom_inputs.size();
    const std::size_t pref_noise_count = pref_noise_inputs.size();
    try {
        destination = compile_expression(
            object, *attribute, ptex_paths, palette_attributes, custom_inputs,
            pref_noise_inputs);
    } catch (const std::exception &error) {
        ptex_paths.resize(map_count);
        custom_inputs.resize(custom_count);
        pref_noise_inputs.resize(pref_noise_count);
        fallback_reasons.push_back(
            object.type + "." + std::string{attribute_name} + ": " +
            error.what());
    }
}

bool supported_runtime_input(std::string_view input) {
    // These are the variables whose fast-float binding is unambiguous without
    // retaining Autodesk patch/evaluation state. Patch u/v/face IDs and ri/rf
    // require the native root sampler and therefore remain fallbacks.
    if (input == "id" || input == "cLength" || input == "cWidth") {
        return true;
    }
    constexpr std::string_view map_prefix{"__nxg_map_"};
    constexpr std::string_view custom_prefix{"__nxg_custom_"};
    constexpr std::string_view pref_noise_prefix{"__nxg_pref_noise_"};
    if (input.starts_with(map_prefix)) {
        input.remove_prefix(map_prefix.size());
    } else if (input.starts_with(custom_prefix)) {
        input.remove_prefix(custom_prefix.size());
    } else if (input.starts_with(pref_noise_prefix)) {
        input.remove_prefix(pref_noise_prefix.size());
    } else {
        return false;
    }
    std::uint32_t index{};
    const auto converted = std::from_chars(
        input.data(), input.data() + input.size(), index);
    return !input.empty() && converted.ec == std::errc{} &&
           converted.ptr == input.data() + input.size() && index < 61u;
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
    constexpr std::string_view prefix{"__nxg_map_"};
    if (name.starts_with(prefix)) {
        name.remove_prefix(prefix.size());
        std::uint32_t index{};
        const auto converted = std::from_chars(
            name.data(), name.data() + name.size(), index);
        if (name.empty() || converted.ec != std::errc{} ||
            converted.ptr != name.data() + name.size() ||
            index >= context.ptex_values.size()) {
            throw std::runtime_error(
                "Classic runtime PTEX input is not bound");
        }
        return context.ptex_values[index];
    }
    constexpr std::string_view custom_prefix{"__nxg_custom_"};
    if (name.starts_with(custom_prefix)) {
        name.remove_prefix(custom_prefix.size());
        std::uint32_t index{};
        const auto converted = std::from_chars(
            name.data(), name.data() + name.size(), index);
        if (name.empty() || converted.ec != std::errc{} ||
            converted.ptr != name.data() + name.size() ||
            index >= context.custom_values.size()) {
            throw std::runtime_error(
                "Classic runtime custom input is not bound");
        }
        return context.custom_values[index];
    }
    constexpr std::string_view pref_noise_prefix{"__nxg_pref_noise_"};
    if (name.starts_with(pref_noise_prefix)) {
        name.remove_prefix(pref_noise_prefix.size());
        std::uint32_t index{};
        const auto converted = std::from_chars(
            name.data(), name.data() + name.size(), index);
        if (name.empty() || converted.ec != std::errc{} ||
            converted.ptr != name.data() + name.size() ||
            index >= context.pref_noise_values.size()) {
            throw std::runtime_error(
                "Classic runtime $Prefg noise input is not bound");
        }
        return context.pref_noise_values[index];
    }
    throw std::runtime_error(
        "Classic runtime variable is not bound: $" + std::string{name});
}

float evaluate_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeContext &context,
    std::span<float> scratch) {
    if (expression.program.inputs.size() > 64u) {
        throw std::runtime_error(
            "Classic runtime expression has too many bound inputs");
    }
    std::array<float, 64u> inputs{};
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
    Vec3 domain_position,
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
        (domain_position + Vec3{0.419276f, 0.184247f, 0.805721f}) *
        domain_scale;
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
        const float tangent_dot = std::clamp(
            dot(prior_tangent, next_tangent), -1.0f, 1.0f);
        if (length_squared(axis) > 1.0e-20f &&
            tangent_dot > -0.999999f) {
            // This is the minimal rotation taking prior_tangent to
            // next_tangent, written without acos/sin/cos. Besides matching
            // the quaternion form used by spline frame transport, it avoids
            // device-libm drift being amplified by several NoiseFX modules.
            const Vec3 first_cross = cross(axis, transported_normal);
            transported_normal = normalize(
                transported_normal + first_cross +
                cross(axis, first_cross) * (1.0f / (1.0f + tangent_dot)));
        }
        const Vec3 normal = transported_normal;
        Vec3 binormal = cross(normal, next_tangent);
        if (length_squared(binormal) > 1.0e-20f) {
            // SgCurve::frame normalizes both returned frame axes. This is
            // material after several NoiseFX passes make the curve sharply
            // bent: using the raw cross product attenuates two noise axes.
            binormal = normalize(binormal);
        }
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

void build_clump_noise_axis(
    std::span<const Vec3> axis,
    const ClassicFloatClumpModule &clump,
    ClassicFloatRuntimeContext context,
    Vec3 domain_position,
    Vec3 surface_normal,
    Vec3 surface_tangent,
    std::uint32_t guide_random_prefix,
    float mask,
    std::span<float> scratch,
    std::vector<Vec3> &output) {
    output.assign(axis.begin(), axis.end());
    context.random_prefix = guide_random_prefix;
    context.has_random_prefix = true;
    const auto evaluate = [&](const ClassicFloatRuntimeExpression &expression) {
        return evaluate_runtime_expression(expression, context, scratch);
    };
    const float raw_noise = evaluate(clump.noise);
    if (!std::isfinite(raw_noise)) {
        throw std::runtime_error(
            "Classic ClumpingFX noise parameter is non-finite");
    }
    const float noise = std::max(raw_noise, 0.0f);
    if (!(noise > 1.0e-5f) || !(mask > 1.0e-4f)) {
        return;
    }
    const float raw_frequency = evaluate(clump.noise_frequency);
    const float raw_correlation = evaluate(clump.noise_correlation);
    if (!std::isfinite(raw_frequency) ||
        !std::isfinite(raw_correlation)) {
        throw std::runtime_error(
            "Classic ClumpingFX noise parameter is non-finite");
    }
    const float frequency = std::max(raw_frequency, 0.0f);
    const float correlation = std::clamp(
        raw_correlation * 0.01f, 0.0f, 1.0f);
    float polyline_length = 0.0f;
    for (std::size_t cv = 1u; cv < axis.size(); ++cv) {
        polyline_length +=
            std::sqrt(length_squared(axis[cv] - axis[cv - 1u]));
    }
    const float effective_frequency = polyline_length > 0.0f
        ? std::max(0.5f / polyline_length, frequency)
        : frequency;
    const float decorrelation = 1.0f - correlation;
    const float domain_scale = 100.0f * decorrelation * decorrelation;
    const Vec3 domain =
        (domain_position + Vec3{0.419276f, 0.184247f, 0.805721f}) *
        domain_scale;
    if (!(length_squared(surface_normal) > 1.0e-20f)) {
        surface_normal = Vec3{0.0f, 1.0f, 0.0f};
    } else {
        surface_normal = normalize(surface_normal);
    }
    if (!(length_squared(surface_tangent) > 1.0e-20f)) {
        surface_tangent = fallback_surface_u(surface_normal);
    } else {
        surface_tangent = normalize(surface_tangent);
    }
    Vec3 transported_u = surface_tangent;
    Vec3 current_tangent = surface_normal;
    const Vec3 first_segment = axis[1u] - axis[0u];
    if (length_squared(first_segment) > 1.0e-20f) {
        current_tangent = normalize(first_segment);
    }
    Vec3 rotation_axis = cross(surface_normal, current_tangent);
    if (length_squared(rotation_axis) > 1.0e-20f) {
        const float angle = std::acos(std::clamp(
            dot(surface_normal, current_tangent), -1.0f, 1.0f));
        transported_u = normalize(rotate_by(
            transported_u, normalize(rotation_axis), angle));
    }
    float travelled = 0.0f;
    for (std::uint32_t cv = 1u; cv < axis.size(); ++cv) {
        travelled += std::sqrt(length_squared(axis[cv] - axis[cv - 1u]));
        Vec3 sample_tangent = current_tangent;
        Vec3 sample_u = transported_u;
        bool advance_frame = false;
        Vec3 next_tangent = current_tangent;
        float turn_angle = 0.0f;
        if (cv + 1u < axis.size()) {
            const Vec3 segment = axis[cv + 1u] - axis[cv];
            if (length_squared(segment) > 1.0e-20f) {
                next_tangent = normalize(segment);
                rotation_axis = cross(current_tangent, next_tangent);
                if (length_squared(rotation_axis) > 1.0e-20f) {
                    rotation_axis = normalize(rotation_axis);
                    turn_angle = std::acos(std::clamp(
                        dot(current_tangent, next_tangent), -1.0f, 1.0f));
                    sample_tangent = normalize(rotate_by(
                        current_tangent, rotation_axis, 0.5f * turn_angle));
                    sample_u = normalize(rotate_by(
                        transported_u, rotation_axis, 0.5f * turn_angle));
                    advance_frame = true;
                } else {
                    current_tangent = next_tangent;
                }
            }
        }
        const Vec3 transported_v = cross(sample_tangent, sample_u);
        context.t = static_cast<float>(cv) /
                    static_cast<float>(axis.size() - 1u);
        const float raw_scale = evaluate(clump.noise_scale);
        if (!std::isfinite(raw_scale)) {
            throw std::runtime_error(
                "Classic ClumpingFX noise scale is non-finite");
        }
        const float scale = std::max(raw_scale, 0.0f);
        const float magnitude = mask * noise * scale;
        if (!std::isfinite(magnitude)) {
            throw std::runtime_error(
                "Classic ClumpingFX noise magnitude is non-finite");
        }
        const float distance = travelled * effective_frequency;
        const float first = xgen_classic_noise_float(
            {domain.x + distance, domain.y, domain.z}) - 0.5f;
        const float second = xgen_classic_noise_float(
            {domain.x, domain.y, domain.z + distance}) - 0.5f;
        output[cv] = output[cv] +
            (sample_u * first + transported_v * second) * magnitude;
        if (advance_frame) {
            transported_u = normalize(rotate_by(
                transported_u, rotation_axis, turn_angle));
            current_tangent = next_tangent;
        }
    }
}

} // namespace

ClassicFloatRuntimePlan compile_xgen_classic_float_runtime_plan(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes) {
    ClassicFloatRuntimePlan result{};
    const auto compile_bound = [&](const ClassicObject &object,
                                   const ClassicAttribute &attribute) {
        return compile_expression(
            object, attribute, result.ptex_paths, palette_attributes,
            result.custom_inputs, result.pref_noise_inputs);
    };
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
                     result.fallback_reasons, result.ptex_paths,
                     palette_attributes, result.custom_inputs,
                     result.pref_noise_inputs);
    compile_optional(*primitive, "width", result.width,
                     result.fallback_reasons, result.ptex_paths,
                     palette_attributes, result.custom_inputs,
                     result.pref_noise_inputs);
    compile_optional(*primitive, "taper", result.taper,
                     result.fallback_reasons, result.ptex_paths,
                     palette_attributes, result.custom_inputs,
                     result.pref_noise_inputs);
    compile_optional(*primitive, "taperStart", result.taper_start,
                     result.fallback_reasons, result.ptex_paths,
                     palette_attributes, result.custom_inputs,
                     result.pref_noise_inputs);
    compile_optional(*primitive, "widthRamp", result.width_ramp,
                     result.fallback_reasons, result.ptex_paths,
                     palette_attributes, result.custom_inputs,
                     result.pref_noise_inputs);
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
                    compile_bound(object, *attribute("mask")),
                    compile_bound(object, *attribute("magnitude")),
                    compile_bound(object, *attribute("magnitudeScale")),
                    compile_bound(object, *attribute("frequency")),
                    compile_bound(object, *attribute("correlation")),
                    compile_bound(object, *attribute("preserveLength")),
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
                "offset", "flatness", "frame"};
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
            if (use_control_maps > 1u) {
                throw std::runtime_error(
                    "ClumpingFXModule useControlMaps must be zero or one");
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
                    compile_bound(
                        object, required_attribute(object, "mask")),
                    compile_bound(
                        object, required_attribute(object, "clump")),
                    compile_bound(
                        object, required_attribute(object, "clumpScale")),
                    compile_bound(
                        object, required_attribute(object, "noise")),
                    compile_bound(
                        object, required_attribute(object, "noiseScale")),
                    compile_bound(
                        object, required_attribute(object, "noiseFrequency")),
                    compile_bound(
                        object, required_attribute(object, "noiseCorrelation")),
                    use_control_maps != 0u};
                bool valid = true;
                valid &= validate_inputs(clump.mask, result.fallback_reasons);
                valid &= validate_inputs(clump.clump, result.fallback_reasons);
                valid &= validate_inputs(
                    clump.clump_scale, result.fallback_reasons);
                valid &= validate_inputs(clump.noise, result.fallback_reasons);
                valid &= validate_inputs(
                    clump.noise_scale, result.fallback_reasons);
                valid &= validate_inputs(
                    clump.noise_frequency, result.fallback_reasons);
                valid &= validate_inputs(
                    clump.noise_correlation, result.fallback_reasons);
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
            ClassicFloatCutModule cut{compile_bound(object, *amount),
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
    std::span<const ClassicClumpRuntimeData> clump_data,
    std::span<const float> runtime_inputs,
    std::span<const Vec3> noise_domain_positions) {
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
    if (!noise_domain_positions.empty() &&
        noise_domain_positions.size() != curves.strand_count) {
        throw std::invalid_argument(
            "Classic NoiseFX needs one domain position per strand");
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
            clump.clump_scale.program.random_call_count != 0u ||
            clump.noise.program.random_call_count != 0u ||
            clump.noise_scale.program.random_call_count != 0u ||
            clump.noise_frequency.program.random_call_count != 0u ||
            clump.noise_correlation.program.random_call_count != 0u;
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
    const std::size_t ptex_count = plan.ptex_paths.size();
    const std::size_t custom_count = plan.custom_inputs.size();
    const std::size_t pref_noise_count = plan.pref_noise_inputs.size();
    const std::size_t input_stride =
        ptex_count + custom_count + pref_noise_count;
    if (input_stride == 0u) {
        if (!runtime_inputs.empty()) {
            throw std::invalid_argument(
                "Classic runtime received an unexpected input table");
        }
    } else if (curves.strand_count >
                   std::numeric_limits<std::size_t>::max() / input_stride ||
               runtime_inputs.size() !=
                   static_cast<std::size_t>(curves.strand_count) *
                       input_stride) {
        throw std::invalid_argument(
            "Classic runtime needs one input row per strand");
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
            data.guide_axes.size() % curves.cvs_per_strand != 0u ||
            data.guide_normals.size() !=
                data.guide_axes.size() / curves.cvs_per_strand ||
            data.guide_tangents.size() != data.guide_normals.size() ||
            (!data.guide_reference_positions.empty() &&
             data.guide_reference_positions.size() !=
                 data.guide_normals.size())) {
            throw std::invalid_argument(
                "Classic ClumpingFX geometry binding is inconsistent");
        }
        if (data.guide_uvs.size() != data.guide_normals.size() ||
            data.guide_face_ids.size() != data.guide_normals.size() ||
            data.guide_random_prefixes.size() != data.guide_normals.size()) {
            throw std::invalid_argument(
                "Classic ClumpingFX guide random binding is inconsistent");
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
    std::vector<Vec3> clump_noise_axis;
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
            &clump.mask, &clump.clump, &clump.clump_scale, &clump.noise,
            &clump.noise_scale, &clump.noise_frequency,
            &clump.noise_correlation};
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
        if (input_stride != 0u) {
            const std::span<const float> row = runtime_inputs.subspan(
                static_cast<std::size_t>(strand) * input_stride,
                input_stride);
            context.ptex_values = row.first(ptex_count);
            context.custom_values = row.subspan(ptex_count, custom_count);
            context.pref_noise_values = row.subspan(
                ptex_count + custom_count, pref_noise_count);
        }
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
        const float raw_width = plan.width
            ? evaluate(*plan.width, context)
            : (radius_scale > 0.0f
                  ? 2.0f * points.front().radius / radius_scale
                  : 0.0f);
        if (!std::isfinite(raw_width)) {
            throw std::runtime_error(
                "Classic width expression produced a non-finite value");
        }
        // XgSplinePrimitive::mkGeometry rejects a primitive before applying
        // FX when either its spline length or authored diameter is below
        // 1e-4. This is observable with extrapolating PTEX fit() expressions:
        // the generator silently omits the strand instead of publishing a
        // zero/negative PrimitiveCache width.
        if (context.c_length < 1.0e-4f || raw_width < 1.0e-4f) {
            keep[strand] = 0u;
            continue;
        }
        context.c_width = raw_width;
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
                    const float raw_mask = evaluate(clump.mask, context);
                    if (!std::isfinite(raw_mask)) {
                        throw std::runtime_error(
                            "Classic ClumpingFX mask is non-finite");
                    }
                    const float mask = std::clamp(raw_mask, 0.0f, 1.0f);
                    if (mask <= 0.0f) { continue; }
                    ClassicFloatRuntimeContext guide_context = context;
                    guide_context.u = data.guide_uvs[guide_index].x;
                    guide_context.v = data.guide_uvs[guide_index].y;
                    guide_context.face_seed = xgen_runtime_face_seed(
                        plan.description_id, plan.description_name,
                        data.guide_face_ids[guide_index]);
                    build_clump_noise_axis(
                        axis, clump, guide_context,
                        data.guide_reference_positions.empty()
                            ? axis.front()
                            : data.guide_reference_positions[guide_index],
                        data.guide_normals[guide_index],
                        data.guide_tangents[guide_index],
                        data.guide_random_prefixes[guide_index], mask, scratch,
                        clump_noise_axis);
                    const float guide_length =
                        clump_guide_lengths[effect.module_index][guide_index];
                    // Clumping always rebuilds an affected curve, even when
                    // the guide is longer and the retained parameter is 1.
                    // That spline-to-CV resampling is observable in Autodesk
                    // output and is not equivalent to leaving the CVs alone.
                    cut_to_length_and_rebuild(
                        points, guide_length, cut_source, resampled);
                    context.c_length = curve_spline_length(points);
                    const float raw_amount = evaluate(clump.clump, context);
                    if (!std::isfinite(raw_amount)) {
                        throw std::runtime_error(
                            "Classic ClumpingFX amount is non-finite");
                    }
                    const float amount = std::clamp(
                        raw_amount, 0.0f, 1.0f);
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
                    for (std::uint32_t cv = 1u;
                         cv < curves.cvs_per_strand; ++cv) {
                        const Vec3 displacement =
                            clump_noise_axis[cv] - axis[cv];
                        points[cv].x += displacement.x;
                        points[cv].y += displacement.y;
                        points[cv].z += displacement.z;
                    }
                    context.c_length = curve_spline_length(points);
                }
            } else if (effect.type == ClassicFloatEffectType::Noise) {
                if (effect.module_index >= plan.noises.size()) {
                    throw std::invalid_argument(
                        "Classic NoiseFX operation index is invalid");
                }
                apply_noise(points, plan.noises[effect.module_index], context,
                            noise_domain_positions.empty()
                                ? Vec3{points.front().x, points.front().y,
                                       points.front().z}
                                : noise_domain_positions[strand],
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
            const float raw_diameter = context.c_width * scale;
            if (!std::isfinite(raw_diameter)) {
                throw std::runtime_error(
                    "Classic width profile produced a non-finite value");
            }
            points[cv].radius =
                0.5f * std::max(raw_diameter, 0.0f) * radius_scale;
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
