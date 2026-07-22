#include "nanoxgen/xgen_classic_ptex.h"

#include "nanoxgen/xgen_ptex.h"

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

ClassicPtexRuntimeData build_xgen_classic_ptex_runtime_data(
    const ClassicFloatRuntimePlan &plan,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots) {
    if (plan.ptex_paths.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Classic runtime PTEX map count exceeds ABI");
    }
    if (roots.roots.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("Classic runtime PTEX strand count exceeds ABI");
    }
    ClassicPtexRuntimeData result{};
    result.strand_count = static_cast<std::uint32_t>(roots.roots.size());
    result.values_per_strand =
        static_cast<std::uint32_t>(plan.ptex_paths.size());
    if (plan.ptex_paths.empty()) { return result; }
    if (roots.roots.size() >
        std::numeric_limits<std::size_t>::max() / plan.ptex_paths.size()) {
        throw std::overflow_error("Classic runtime PTEX table is too large");
    }
    std::vector<std::unique_ptr<XgenPtexMap>> maps;
    maps.reserve(plan.ptex_paths.size());
    for (const std::string &path : plan.ptex_paths) {
        maps.push_back(std::make_unique<XgenPtexMap>(
            resolve_map(path, description_directory, patch_name)));
    }
    result.values.resize(roots.roots.size() * maps.size());
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
            result.values[strand * maps.size() + map] = maps[map]->sample(
                face, root.uv.x, root.uv.y, 0u, options);
        }
    }
    return result;
}

} // namespace nanoxgen
