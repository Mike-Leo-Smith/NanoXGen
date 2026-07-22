#include "nanoxgen/xgen_classic_ptex.h"

#include "nanoxgen/xgen_ptex.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace nanoxgen {
namespace {

std::filesystem::path resolve_map(
    std::string_view value, const std::filesystem::path &description_directory,
    std::string_view patch_name) {
    constexpr std::string_view prefix{"${DESC}"};
    std::filesystem::path result;
    if (value.starts_with(prefix)) {
        value.remove_prefix(prefix.size());
        while (value.starts_with('/')) { value.remove_prefix(1u); }
        result = description_directory /
                 std::filesystem::path{std::string{value}};
    } else {
        result = std::filesystem::path{std::string{value}};
    }
    if (result.extension() != ".ptx") {
        result /= std::string{patch_name} + ".ptx";
    }
    if (!std::filesystem::is_regular_file(result)) {
        throw std::runtime_error(
            "Classic runtime PTEX map does not exist: " + result.string());
    }
    return result;
}

} // namespace

ClassicRuntimeInputData build_xgen_classic_runtime_input_data(
    const ClassicFloatRuntimePlan &plan,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots) {
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
    std::vector<std::unique_ptr<XgenPtexMap>> maps;
    maps.reserve(plan.ptex_paths.size());
    for (const std::string &path : plan.ptex_paths) {
        maps.push_back(std::make_unique<XgenPtexMap>(
            resolve_map(path, description_directory, patch_name)));
    }
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
    std::vector<float> custom_scratch(custom_scratch_size);
    result.values.resize(roots.roots.size() * value_count);
    XgenPtexSampleOptions options{};
    options.filter = XgenPtexFilter::Point;
    for (std::size_t strand = 0u; strand < roots.roots.size(); ++strand) {
        const RootSample &root = roots.roots[strand];
        const std::uint32_t face = root.surface_face_id == kInvalidIndex
            ? root.triangle_index : root.surface_face_id;
        for (std::size_t map = 0u; map < maps.size(); ++map) {
            if (face >= maps[map]->info().face_count) {
                throw std::runtime_error(
                    "Classic runtime PTEX map has fewer faces than the patch");
            }
            result.values[strand * value_count + map] = maps[map]->sample(
                face, root.uv.x, root.uv.y, 0u, options);
        }
        for (std::size_t custom = 0u;
             custom < plan.custom_inputs.size(); ++custom) {
            const ClassicFloatCustomInput &input = plan.custom_inputs[custom];
            const std::array<float, 1u> id{
                static_cast<float>(roots.primitive_ids[strand])};
            const std::span<const float> values = input.program.inputs.empty()
                ? std::span<const float>{}
                : std::span<const float>{id};
            result.values[strand * value_count + maps.size() + custom] =
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
            const float frequency = plan.pref_noise_inputs[noise].frequency;
            const Vec3 reference = roots.reference_positions[strand];
            result.values[
                strand * value_count + maps.size() +
                plan.custom_inputs.size() + noise] =
                xgen_classic_noise_float(reference * frequency);
        }
    }
    return result;
}

} // namespace nanoxgen
