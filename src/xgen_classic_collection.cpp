#include "nanoxgen/xgen_classic_collection.h"

#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_roots.h"

#include <algorithm>
#include <stdexcept>

namespace nanoxgen {

ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options) {
    const ClassicCollection collection =
        load_xgen_classic_collection(collection_path);
    return build_xgen_classic_collection_execution_plan(
        collection, collection_path, archive_path, descriptions_root,
        options);
}

ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options) {
    if (options.max_host_workers == 0u) {
        throw std::invalid_argument(
            "Classic collection host worker limit must be nonzero");
    }
    ClassicCollectionExecutionPlan result{};
    result.collection_path = collection_path;
    result.archive_path = archive_path;
    result.descriptions_root = descriptions_root;
    result.descriptions.reserve(collection.descriptions.size());
    for (const ClassicDescription &description : collection.descriptions) {
        ClassicCollectionExecutionDescription output{};
        output.name = description.name;
        output.runtime = compile_xgen_classic_float_runtime_plan(
            description, collection.palette_attributes);
        if (!output.runtime.lowering_complete()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' needs fallback: " +
                output.runtime.fallback_reasons.front());
        }
        const std::uint32_t cvs = output.runtime.fx_cv_count;
        output.surface = build_xgen_classic_alembic_asset_input(
            description, archive_path);
        output.roots = build_xgen_classic_random_root_plan(
            description, output.surface,
            descriptions_root / description.name);
        if (output.roots.roots.empty() ||
            output.roots.influence_offsets.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no generated roots");
        }
        if (description.patches.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no patch");
        }
        output.runtime_inputs = build_xgen_classic_runtime_input_data(
            output.runtime, descriptions_root / description.name,
            description.patches.front().name, output.roots);
        output.clumps = build_xgen_classic_clump_runtime_data_parallel(
            description, output.surface,
            descriptions_root / description.name, output.roots,
            output.runtime, cvs, options.max_host_workers);
        output.runtime.effects.resize(std::min<std::size_t>(
            options.effect_count, output.runtime.effects.size()));
        output.rebuilt_guides = rebuild_xgen_classic_guides_for_device(
            output.surface.asset, cvs);
        result.descriptions.emplace_back(std::move(output));
    }
    if (result.descriptions.empty()) {
        throw std::runtime_error(
            "Classic collection main file has no descriptions");
    }
    return result;
}

} // namespace nanoxgen
