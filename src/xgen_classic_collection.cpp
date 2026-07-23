#include "nanoxgen/xgen_classic_collection.h"

#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_roots.h"

#include <algorithm>
#include <memory>
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
    if (collection.descriptions.empty()) {
        throw std::runtime_error(
            "Classic collection main file has no descriptions");
    }
    std::unique_ptr<NanoXGenContext> owned_context;
    NanoXGenContext *context = options.context;
    if (!context) {
        owned_context = std::make_unique<NanoXGenContext>();
        context = owned_context.get();
    }
    TaskExecutor &executor = context->executor();
    ClassicCollectionExecutionPlan result{};
    result.collection_path = collection_path;
    result.archive_path = archive_path;
    result.descriptions_root = descriptions_root;
    result.descriptions.resize(collection.descriptions.size());
    const auto prepare_description = [&](std::size_t index) {
        const ClassicDescription &description =
            collection.descriptions[index];
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
            description.patches.front().name, output.roots,
            context);
        output.clumps = build_xgen_classic_clump_runtime_data_parallel(
            description, output.surface,
            descriptions_root / description.name, output.roots,
            output.runtime, cvs, context);
        output.runtime.effects.resize(std::min<std::size_t>(
            options.effect_count, output.runtime.effects.size()));
        output.rebuilt_guides = rebuild_xgen_classic_guides_for_device(
            output.surface.asset, cvs);
        result.descriptions[index] = std::move(output);
    };
    result.context_worker_count = executor.worker_count();
    if (collection.descriptions.size() <= 1u ||
        executor.worker_count() <= 1u) {
        for (std::size_t index = 0u;
             index < collection.descriptions.size(); ++index) {
            prepare_description(index);
        }
    } else {
        executor.parallel_for(
            collection.descriptions.size(), prepare_description);
    }
    return result;
}

} // namespace nanoxgen
