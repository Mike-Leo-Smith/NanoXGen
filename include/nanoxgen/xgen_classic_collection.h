#pragma once

#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_alembic.h"
#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace nanoxgen {

struct ClassicCollectionExecutionOptions {
    std::uint32_t effect_count{
        std::numeric_limits<std::uint32_t>::max()};
    // Descriptions are independent after the master file is parsed. Zero
    // partitions the machine's logical threads across max_host_workers;
    // positive values explicitly bound collection-wide concurrency.
    std::size_t max_description_workers{};
    // Per-description ClumpingFX module concurrency. This is separate from
    // max_description_workers because PTEX binding also has internal workers.
    std::size_t max_host_workers{8u};
};

struct ClassicCollectionExecutionDescription {
    std::string name;
    ClassicFloatRuntimePlan runtime;
    ClassicAlembicAssetInput surface;
    ClassicRootPlan roots;
    ClassicRuntimeInputData runtime_inputs;
    std::vector<ClassicClumpRuntimeData> clumps;
    std::vector<Vec3> rebuilt_guides;
};

// Backend-neutral host plan built from one Classic collection main file.
// Render-patch geometry and the description data root are explicit because a
// renderer normally supplies them through its scene/package resolver.
struct ClassicCollectionExecutionPlan {
    std::filesystem::path collection_path;
    std::filesystem::path archive_path;
    std::filesystem::path descriptions_root;
    std::size_t description_worker_count{};
    std::vector<ClassicCollectionExecutionDescription> descriptions;
};

[[nodiscard]] ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options = {});

[[nodiscard]] ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options = {});

} // namespace nanoxgen
