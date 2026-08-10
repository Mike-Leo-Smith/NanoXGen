#include "nanoxgen/xgen_classic_ptex.h"

#include "nanoxgen/xgen_ptex.h"
#include "xgen_classic_path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace nanoxgen {
namespace {

std::filesystem::path resolve_map(
    std::string_view value, const std::filesystem::path &description_directory,
    std::string_view patch_name) {
    if (!detail::classic_safe_component(patch_name)) {
        throw std::runtime_error(
            "Classic runtime patch name is not a safe path component");
    }
    const std::string fallback_filename =
        std::string{patch_name} + ".ptx";
    const std::filesystem::path result =
        detail::resolve_classic_description_file(
            value, description_directory, fallback_filename, ".ptx");
    if (!std::filesystem::is_regular_file(result)) {
        throw std::runtime_error(
            "Classic runtime PTEX map does not exist: " + result.string());
    }
    return result;
}

std::string strip_expression_comments(std::string_view source) {
    std::string result;
    result.reserve(source.size());
    bool quoted = false;
    char quote{};
    for (std::size_t index = 0u; index < source.size();) {
        const char c = source[index];
        if (!quoted && c == '\\' && index + 1u < source.size() &&
            source[index + 1u] == 'n') {
            result.push_back('\n');
            index += 2u;
            continue;
        }
        if (!quoted && c == '#') {
            while (index < source.size()) {
                if (source[index] == '\n') {
                    result.push_back('\n');
                    ++index;
                    break;
                }
                if (source[index] == '\\' && index + 1u < source.size() &&
                    source[index + 1u] == 'n') {
                    result.push_back('\n');
                    index += 2u;
                    break;
                }
                ++index;
            }
            continue;
        }
        result.push_back(c);
        if (quoted) {
            if (c == quote &&
                (index == 0u || source[index - 1u] != '\\')) {
                quoted = false;
            }
        } else if (c == '\'' || c == '"') {
            quoted = true;
            quote = c;
        }
        ++index;
    }
    return result;
}

std::optional<std::string> direct_map_path(
    std::string_view authored_source, std::string_view attribute_name) {
    const std::string storage = strip_expression_comments(authored_source);
    const std::string_view source{storage};
    std::size_t cursor{};
    const auto skip_space = [&] {
        while (cursor < source.size() &&
               std::isspace(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }
    };
    const auto parse_variable = [&]() -> std::string {
        skip_space();
        if (cursor >= source.size() || source[cursor++] != '$') {
            return {};
        }
        const std::size_t begin = cursor;
        while (cursor < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[cursor])) ||
                source[cursor] == '_')) {
            ++cursor;
        }
        return std::string{source.substr(begin, cursor - begin)};
    };

    skip_space();
    const std::size_t start = cursor;
    std::string destination = parse_variable();
    skip_space();
    if (destination.empty() || cursor >= source.size() ||
        source[cursor] != '=') {
        destination.clear();
        cursor = start;
    } else {
        ++cursor;
    }
    skip_space();
    if (source.substr(cursor, 3u) != "map") {
        return std::nullopt;
    }
    cursor += 3u;
    skip_space();
    if (cursor >= source.size() || source[cursor++] != '(') {
        throw std::runtime_error(
            "Classic custom color '" + std::string{attribute_name} +
            "' has an invalid map() call");
    }
    skip_space();
    if (cursor >= source.size() ||
        (source[cursor] != '\'' && source[cursor] != '"')) {
        throw std::runtime_error(
            "Classic custom color '" + std::string{attribute_name} +
            "' map path must be quoted");
    }
    const char quote = source[cursor++];
    const std::size_t path_begin = cursor;
    while (cursor < source.size() && source[cursor] != quote) { ++cursor; }
    if (cursor >= source.size()) {
        throw std::runtime_error(
            "Classic custom color '" + std::string{attribute_name} +
            "' has an unterminated map path");
    }
    const std::string path{source.substr(path_begin, cursor - path_begin)};
    ++cursor;
    skip_space();
    if (cursor >= source.size() || source[cursor++] != ')') {
        throw std::runtime_error(
            "Classic custom color '" + std::string{attribute_name} +
            "' map() is missing a closing parenthesis");
    }
    skip_space();
    if (cursor < source.size() && source[cursor] == ';') {
        ++cursor;
    }
    skip_space();
    if (!destination.empty()) {
        const std::string returned = parse_variable();
        if (returned != destination) {
            throw std::runtime_error(
                "Classic custom color '" + std::string{attribute_name} +
                "' applies unsupported vector operations");
        }
        skip_space();
        if (cursor < source.size() && source[cursor] == ';') {
            ++cursor;
        }
        skip_space();
    }
    if (cursor != source.size() || path.empty()) {
        throw std::runtime_error(
            "Classic custom color '" + std::string{attribute_name} +
            "' applies unsupported vector operations");
    }
    return path;
}

struct AuthoredCustomAttribute {
    const ClassicAttribute *attribute{};
    std::string_view object_type;
};

std::vector<AuthoredCustomAttribute> collect_custom_attributes(
    const ClassicDescription &description,
    std::string_view prefix) {
    std::vector<AuthoredCustomAttribute> result;
    for (const ClassicObject &object : description.objects) {
        if (object.type != "RendermanRenderer") { continue; }
        for (const ClassicAttribute &attribute : object.attributes) {
            if (attribute.name.starts_with(prefix) &&
                attribute.name.size() > prefix.size()) {
                result.push_back({&attribute, object.type});
            }
        }
    }
    return result;
}

} // namespace

void validate_xgen_classic_uniform_primvar_declarations(
    const ClassicDescription &description) {
    constexpr std::array<std::string_view, 2u> supported_prefixes{
        "custom_float_", "custom_color_"};
    constexpr std::array<std::string_view, 3u> unsupported_prefixes{
        "custom_vector_", "custom_normal_", "custom_point_"};
    std::unordered_set<std::string> names;
    for (const ClassicObject &object : description.objects) {
        if (object.type != "RendermanRenderer") { continue; }
        for (const ClassicAttribute &attribute : object.attributes) {
            const std::string_view authored_name{attribute.name};
            if (authored_name.starts_with("custom__")) {
                continue;
            }
            if (!authored_name.starts_with("custom_")) {
                continue;
            }

            std::string_view primvar_name;
            for (const std::string_view prefix : supported_prefixes) {
                if (authored_name.starts_with(prefix)) {
                    primvar_name = authored_name.substr(prefix.size());
                    break;
                }
            }
            if (primvar_name.empty()) {
                const auto unsupported = std::find_if(
                    unsupported_prefixes.begin(),
                    unsupported_prefixes.end(),
                    [&](std::string_view prefix) {
                        return authored_name.starts_with(prefix);
                    });
                const std::string reason =
                    unsupported != unsupported_prefixes.end()
                    ? "uses an unsupported uniform type"
                    : "has an unsupported declaration";
                throw std::runtime_error(
                    "Classic custom primvar '" + attribute.name + "' " +
                    reason);
            }
            if (primvar_name.find('[') != std::string_view::npos ||
                primvar_name.find(']') != std::string_view::npos) {
                throw std::runtime_error(
                    "Classic custom primvar arrays are unsupported: " +
                    attribute.name);
            }
            if (!names.emplace(primvar_name).second) {
                throw std::runtime_error(
                    "Classic custom primvar name is duplicated: " +
                    std::string{primvar_name});
            }
        }
    }
}

ClassicRuntimeInputData build_xgen_classic_runtime_input_data(
    const ClassicFloatRuntimePlan &plan,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    NanoXGenContext *context) {
    const std::size_t value_count =
        plan.ptex_paths.size() + plan.custom_inputs.size() +
        plan.pref_noise_inputs.size();
    if (value_count >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Classic runtime input count exceeds ABI");
    }
    if (roots.roots.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Classic runtime PTEX strand count exceeds ABI");
    }
    ClassicRuntimeInputData result{};
    result.strand_count = static_cast<std::uint32_t>(roots.roots.size());
    result.values_per_strand =
        static_cast<std::uint32_t>(value_count);
    if (value_count == 0u) { return result; }
    if (roots.roots.size() >
        std::numeric_limits<std::size_t>::max() / value_count) {
        throw std::overflow_error("Classic runtime input table is too large");
    }
    std::vector<std::filesystem::path> map_paths;
    map_paths.reserve(plan.ptex_paths.size());
    for (const std::string &path : plan.ptex_paths) {
        map_paths.push_back(
            resolve_map(path, description_directory, patch_name));
    }
    const auto open_maps = [&] {
        std::vector<std::unique_ptr<XgenPtexMap>> maps;
        maps.reserve(map_paths.size());
        for (const std::filesystem::path &path : map_paths) {
            maps.push_back(std::make_unique<XgenPtexMap>(path));
        }
        return maps;
    };
    if (!plan.custom_inputs.empty() &&
        (roots.primitive_ids.size() != roots.roots.size() ||
         roots.random_prefixes.size() != roots.roots.size())) {
        throw std::runtime_error(
            "Classic root inputs need primitive IDs and random prefixes");
    }
    if (!plan.pref_noise_inputs.empty() &&
        roots.reference_positions.size() != roots.roots.size()) {
        throw std::runtime_error(
            "Classic $Prefg noise needs one reference position per root");
    }
    std::size_t custom_scratch_size = 0u;
    for (const ClassicFloatCustomInput &custom : plan.custom_inputs) {
        custom_scratch_size = std::max(
            custom_scratch_size, custom.program.instructions.size());
        for (const std::string &input : custom.program.inputs) {
            if (input != "id") {
                throw std::runtime_error(
                    "Classic palette custom input uses unsupported variable $" +
                    input);
            }
        }
    }
    result.values.resize(roots.roots.size() * value_count);
    const auto sample_range = [&](
        const std::vector<std::unique_ptr<XgenPtexMap>> &worker_maps,
        std::size_t begin, std::size_t end) {
        std::vector<float> custom_scratch(custom_scratch_size);
        XgenPtexSampleOptions options{};
        options.filter = XgenPtexFilter::Point;
        for (std::size_t strand = begin; strand < end; ++strand) {
            const RootSample &root = roots.roots[strand];
            const std::uint32_t face = root.surface_face_id == kInvalidIndex
                ? root.triangle_index : root.surface_face_id;
            for (std::size_t map = 0u; map < worker_maps.size(); ++map) {
                if (face >= worker_maps[map]->info().face_count) {
                    throw std::runtime_error(
                        "Classic runtime PTEX map has fewer faces than the patch");
                }
                result.values[strand * value_count + map] =
                    worker_maps[map]->sample(
                        face, root.uv.x, root.uv.y, 0u, options);
            }
            for (std::size_t custom = 0u;
                 custom < plan.custom_inputs.size(); ++custom) {
                const ClassicFloatCustomInput &input =
                    plan.custom_inputs[custom];
                const std::array<float, 1u> id{
                    static_cast<float>(roots.primitive_ids[strand])};
                const std::span<const float> values =
                    input.program.inputs.empty()
                    ? std::span<const float>{}
                    : std::span<const float>{id};
                result.values[
                    strand * value_count + worker_maps.size() + custom] =
                    evaluate_xgen_scalar_expression_float(
                        input.program,
                        {values, root.uv.x, root.uv.y,
                         xgen_runtime_face_seed(
                             plan.description_id, plan.description_name, face),
                         0.0f, roots.random_prefixes[strand], true},
                        custom_scratch);
            }
            for (std::size_t noise = 0u;
                 noise < plan.pref_noise_inputs.size(); ++noise) {
                const float frequency =
                    plan.pref_noise_inputs[noise].frequency;
                const Vec3 reference = roots.reference_positions[strand];
                result.values[
                    strand * value_count + worker_maps.size() +
                    plan.custom_inputs.size() + noise] =
                    xgen_classic_noise_float(reference * frequency);
            }
        }
    };
    constexpr std::size_t minimum_strands_per_task = 16384u;
    constexpr std::size_t chunk_size = 4096u;
    const std::size_t useful_tasks = std::max<std::size_t>(
        1u, (roots.roots.size() + minimum_strands_per_task - 1u) /
                minimum_strands_per_task);
    const std::size_t capacity =
        context ? context->worker_count() : available_worker_count();
    const std::size_t task_count =
        std::min(capacity, useful_tasks);
    if (task_count <= 1u) {
        const auto maps = open_maps();
        sample_range(maps, 0u, roots.roots.size());
    } else {
        std::unique_ptr<NanoXGenContext> owned_context;
        if (!context) {
            owned_context = std::make_unique<NanoXGenContext>(capacity);
            context = owned_context.get();
        }
        std::atomic_size_t next_strand{};
        context->executor().parallel_for(task_count, [&](std::size_t) {
            const auto worker_maps = open_maps();
            while (true) {
                const std::size_t begin = next_strand.fetch_add(
                    chunk_size, std::memory_order_relaxed);
                if (begin >= roots.roots.size()) { return; }
                sample_range(
                    worker_maps, begin,
                    std::min(begin + chunk_size, roots.roots.size()));
            }
        });
    }
    return result;
}

std::vector<ClassicUniformFloatPrimvar>
build_xgen_classic_uniform_float_primvars(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes,
    std::uint32_t description_id,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    NanoXGenContext *context,
    float frame) {
    if (!std::isfinite(frame)) {
        throw std::invalid_argument(
            "Classic custom float frame must be finite");
    }
    validate_xgen_classic_uniform_primvar_declarations(description);
    constexpr std::string_view prefix{"custom_float_"};
    ClassicFloatRuntimePlan input_plan{};
    input_plan.description_name = description.name;
    input_plan.description_id = description_id;
    std::vector<ClassicUniformFloatPrimvar> result;
    std::vector<ClassicFloatRuntimeExpression> expressions;
    std::unordered_set<std::string> names;
    for (const AuthoredCustomAttribute &authored :
         collect_custom_attributes(description, prefix)) {
        const ClassicAttribute &attribute = *authored.attribute;
        const std::string name = attribute.name.substr(prefix.size());
        if (!names.emplace(name).second) {
            throw std::runtime_error(
                "Classic custom float primvar is duplicated: " + name);
        }
        ClassicFloatRuntimeExpression expression =
            compile_xgen_classic_uniform_float_expression(
                description, authored.object_type, attribute,
                palette_attributes,
                input_plan.ptex_paths, input_plan.custom_inputs,
                input_plan.pref_noise_inputs);
        for (const std::string &input : expression.program.inputs) {
            if (input == "id" || input == "frame" ||
                input.starts_with("__nxg_map_") ||
                input.starts_with("__nxg_custom_") ||
                input.starts_with("__nxg_pref_noise_")) {
                continue;
            }
            throw std::runtime_error(
                "Classic custom float '" + name +
                "' uses unsupported runtime variable $" + input);
        }
        result.push_back({name, std::vector<float>(roots.roots.size())});
        expressions.emplace_back(std::move(expression));
    }
    if (result.empty() || roots.roots.empty()) { return result; }

    const ClassicRuntimeInputData inputs =
        build_xgen_classic_runtime_input_data(
            input_plan, description_directory, patch_name, roots, context);
    const std::size_t ptex_count = input_plan.ptex_paths.size();
    const std::size_t custom_count = input_plan.custom_inputs.size();
    const std::size_t noise_count = input_plan.pref_noise_inputs.size();
    const std::size_t values_per_strand =
        ptex_count + custom_count + noise_count;
    std::size_t scratch_size{};
    for (const auto &expression : expressions) {
        scratch_size = std::max(
            scratch_size, expression.program.instructions.size());
    }
    const auto evaluate_range = [&](std::size_t begin, std::size_t end) {
        std::vector<float> scratch(scratch_size);
        for (std::size_t strand = begin; strand < end; ++strand) {
            const RootSample &root = roots.roots[strand];
            const std::uint32_t face =
                root.surface_face_id == kInvalidIndex
                ? root.triangle_index
                : root.surface_face_id;
            const std::span<const float> row = values_per_strand == 0u
                ? std::span<const float>{}
                : std::span<const float>{inputs.values}.subspan(
                      strand * values_per_strand, values_per_strand);
            const ClassicFloatRuntimeContext runtime_context{
                roots.primitive_ids.at(strand),
                root.uv.x,
                root.uv.y,
                xgen_runtime_face_seed(
                    description_id, patch_name, face),
                0.0f,
                0.0f,
                0.0f,
                frame,
                roots.random_prefixes.at(strand),
                true,
                row.first(ptex_count),
                row.subspan(ptex_count, custom_count),
                row.last(noise_count)};
            for (std::size_t primvar = 0u;
                 primvar < expressions.size(); ++primvar) {
                const auto &expression = expressions[primvar];
                std::array<float, 64u> bound{};
                if (expression.program.inputs.size() > bound.size()) {
                    throw std::runtime_error(
                        "Classic custom float expression has too many inputs");
                }
                for (std::size_t index = 0u;
                     index < expression.program.inputs.size(); ++index) {
                    const std::string &input =
                        expression.program.inputs[index];
                    if (input == "id") {
                        bound[index] =
                            static_cast<float>(runtime_context.id);
                        continue;
                    }
                    if (input == "frame") {
                        bound[index] = runtime_context.frame;
                        continue;
                    }
                    const auto parse_external =
                        [&](std::string_view input_prefix,
                            std::span<const float> values) -> bool {
                        if (!input.starts_with(input_prefix)) {
                            return false;
                        }
                        const std::string_view suffix{
                            input.data() + input_prefix.size(),
                            input.size() - input_prefix.size()};
                        std::uint32_t value_index{};
                        const auto converted = std::from_chars(
                            suffix.data(), suffix.data() + suffix.size(),
                            value_index);
                        if (suffix.empty() ||
                            converted.ec != std::errc{} ||
                            converted.ptr !=
                                suffix.data() + suffix.size() ||
                            value_index >= values.size()) {
                            throw std::runtime_error(
                                "Classic custom float external input is "
                                "invalid");
                        }
                        bound[index] = values[value_index];
                        return true;
                    };
                    if (parse_external(
                            "__nxg_map_",
                            runtime_context.ptex_values) ||
                        parse_external(
                            "__nxg_custom_",
                            runtime_context.custom_values) ||
                        parse_external(
                            "__nxg_pref_noise_",
                            runtime_context.pref_noise_values)) {
                        continue;
                    }
                    throw std::runtime_error(
                        "Classic custom float input is not bound");
                }
                result[primvar].values[strand] =
                    evaluate_xgen_scalar_expression_float(
                        expression.program,
                        {std::span{bound}.first(
                             expression.program.inputs.size()),
                         runtime_context.u,
                         runtime_context.v,
                         runtime_context.face_seed,
                         runtime_context.t,
                         runtime_context.random_prefix,
                         runtime_context.has_random_prefix},
                        scratch);
            }
        }
    };
    constexpr std::size_t minimum_strands_per_task = 16384u;
    constexpr std::size_t chunk_size = 4096u;
    const std::size_t useful_tasks = std::max<std::size_t>(
        1u, (roots.roots.size() + minimum_strands_per_task - 1u) /
                minimum_strands_per_task);
    const std::size_t capacity =
        context ? context->worker_count() : available_worker_count();
    const std::size_t task_count = std::min(capacity, useful_tasks);
    if (task_count <= 1u) {
        evaluate_range(0u, roots.roots.size());
    } else {
        std::unique_ptr<NanoXGenContext> owned_context;
        if (!context) {
            owned_context = std::make_unique<NanoXGenContext>(capacity);
            context = owned_context.get();
        }
        std::atomic_size_t next_strand{};
        context->executor().parallel_for(task_count, [&](std::size_t) {
            while (true) {
                const std::size_t begin = next_strand.fetch_add(
                    chunk_size, std::memory_order_relaxed);
                if (begin >= roots.roots.size()) { return; }
                evaluate_range(
                    begin,
                    std::min(begin + chunk_size, roots.roots.size()));
            }
        });
    }
    return result;
}

std::vector<ClassicUniformColorPrimvar>
build_xgen_classic_uniform_color_primvars(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    ClassicUniformColorExpressionEvaluator expression_evaluator,
    NanoXGenContext *context) {
    validate_xgen_classic_uniform_primvar_declarations(description);
    constexpr std::string_view prefix{"custom_color_"};
    std::vector<std::optional<std::filesystem::path>> map_paths;
    std::vector<const ClassicAttribute *> expressions;
    std::vector<ClassicUniformColorPrimvar> result;
    std::unordered_set<std::string> names;
    for (const AuthoredCustomAttribute &authored :
         collect_custom_attributes(description, prefix)) {
        const ClassicAttribute &attribute = *authored.attribute;
        const std::string name = attribute.name.substr(prefix.size());
        if (!names.emplace(name).second) {
            throw std::runtime_error(
                "Classic custom color primvar is duplicated: " + name);
        }
        const std::optional<std::string> path =
            direct_map_path(attribute.value, attribute.name);
        if (path) {
            map_paths.push_back(resolve_map(
                *path, description_directory, patch_name));
            expressions.push_back(nullptr);
        } else {
            if (!expression_evaluator) {
                throw std::runtime_error(
                    "Classic custom color '" + attribute.name +
                    "' needs a typed expression evaluator");
            }
            map_paths.push_back(std::nullopt);
            expressions.push_back(&attribute);
        }
        result.push_back({
            name, std::vector<Vec3>(roots.roots.size())});
    }
    if (result.empty() || roots.roots.empty()) { return result; }

    if (std::find_if(
            expressions.begin(), expressions.end(),
            [](const ClassicAttribute *attribute) {
                return attribute != nullptr;
            }) != expressions.end() &&
        roots.reference_positions.size() != roots.roots.size()) {
        throw std::runtime_error(
            "Classic custom color expressions need one $Pref per root");
    }
    for (std::size_t index = 0u; index < expressions.size(); ++index) {
        if (const ClassicAttribute *attribute = expressions[index]) {
            expression_evaluator(
                attribute->value, attribute->name, palette_attributes,
                roots, result[index].values, context);
        }
    }

    const auto open_maps = [&] {
        std::vector<std::unique_ptr<XgenPtexMap>> maps;
        maps.reserve(map_paths.size());
        for (const auto &path : map_paths) {
            maps.push_back(path
                ? std::make_unique<XgenPtexMap>(*path)
                : nullptr);
        }
        return maps;
    };
    const auto sample_range = [&](
        const std::vector<std::unique_ptr<XgenPtexMap>> &maps,
        std::size_t begin, std::size_t end) {
        XgenPtexSampleOptions options{};
        options.filter = XgenPtexFilter::Point;
        for (std::size_t strand = begin; strand < end; ++strand) {
            const RootSample &root = roots.roots[strand];
            const std::uint32_t face =
                root.surface_face_id == kInvalidIndex
                ? root.triangle_index
                : root.surface_face_id;
            for (std::size_t map = 0u; map < maps.size(); ++map) {
                if (!maps[map]) { continue; }
                const auto &ptex = *maps[map];
                if (face >= ptex.info().face_count) {
                    throw std::runtime_error(
                        "Classic color PTEX map has fewer faces than the patch");
                }
                const float r = ptex.sample(
                    face, root.uv.x, root.uv.y, 0u, options);
                const float g = ptex.info().channel_count > 1u
                    ? ptex.sample(face, root.uv.x, root.uv.y, 1u, options)
                    : r;
                const float b = ptex.info().channel_count > 2u
                    ? ptex.sample(face, root.uv.x, root.uv.y, 2u, options)
                    : r;
                result[map].values[strand] = {r, g, b};
            }
        }
    };

    constexpr std::size_t minimum_strands_per_task = 16384u;
    constexpr std::size_t chunk_size = 4096u;
    const std::size_t useful_tasks = std::max<std::size_t>(
        1u, (roots.roots.size() + minimum_strands_per_task - 1u) /
                minimum_strands_per_task);
    const std::size_t capacity =
        context ? context->worker_count() : available_worker_count();
    const std::size_t task_count = std::min(capacity, useful_tasks);
    if (task_count <= 1u) {
        const auto maps = open_maps();
        sample_range(maps, 0u, roots.roots.size());
    } else {
        std::unique_ptr<NanoXGenContext> owned_context;
        if (!context) {
            owned_context = std::make_unique<NanoXGenContext>(capacity);
            context = owned_context.get();
        }
        std::atomic_size_t next_strand{};
        context->executor().parallel_for(task_count, [&](std::size_t) {
            const auto maps = open_maps();
            while (true) {
                const std::size_t begin = next_strand.fetch_add(
                    chunk_size, std::memory_order_relaxed);
                if (begin >= roots.roots.size()) { return; }
                sample_range(
                    maps, begin,
                    std::min(begin + chunk_size, roots.roots.size()));
            }
        });
    }
    return result;
}

} // namespace nanoxgen
