#include "nanoxgen/xgen_classic_ptex.h"

#include "nanoxgen/xgen_ptex.h"

#include <algorithm>
#include <array>
#include <atomic>
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

} // namespace nanoxgen
