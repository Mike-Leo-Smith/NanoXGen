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
