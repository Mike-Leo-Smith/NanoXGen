#include "nanoxgen/xgen_classic_collection.h"

#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_roots.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

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
            description.patches.front().name, output.roots);
        output.clumps = build_xgen_classic_clump_runtime_data_parallel(
            description, output.surface,
            descriptions_root / description.name, output.roots,
            output.runtime, cvs, options.max_host_workers);
        output.runtime.effects.resize(std::min<std::size_t>(
            options.effect_count, output.runtime.effects.size()));
        output.rebuilt_guides = rebuild_xgen_classic_guides_for_device(
            output.surface.asset, cvs);
        result.descriptions[index] = std::move(output);
    };
    const std::size_t hardware_workers = std::max<std::size_t>(
        std::thread::hardware_concurrency(), 1u);
    const std::size_t description_limit =
        options.max_description_workers == 0u
        ? std::max<std::size_t>(
            hardware_workers / options.max_host_workers, 1u)
        : options.max_description_workers;
    const std::size_t worker_count = std::min(
        description_limit, collection.descriptions.size());
    result.description_worker_count = worker_count;
    if (worker_count <= 1u) {
        for (std::size_t index = 0u;
             index < collection.descriptions.size(); ++index) {
            prepare_description(index);
        }
    } else {
        std::atomic_size_t next_description{};
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0u; worker < worker_count; ++worker) {
            workers.emplace_back(std::async(
                std::launch::async,
                [&] {
                    while (true) {
                        const std::size_t index =
                            next_description.fetch_add(
                                1u, std::memory_order_relaxed);
                        if (index >= collection.descriptions.size()) {
                            return;
                        }
                        prepare_description(index);
                    }
                }));
        }
        for (std::future<void> &worker : workers) { worker.get(); }
    }
    if (result.descriptions.empty()) {
        throw std::runtime_error(
            "Classic collection main file has no descriptions");
    }
    return result;
}

} // namespace nanoxgen
