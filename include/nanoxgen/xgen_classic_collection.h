#pragma once

#include "nanoxgen/context.h"
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
    // Optional context shared by description, PTEX, clump, and later JIT work.
    // A null context creates an affinity-aware pool for this call and releases
    // it on return. A supplied context must outlive this call.
    NanoXGenContext *context{};
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
    std::size_t context_worker_count{1u};
    std::vector<ClassicCollectionExecutionDescription> descriptions;
};

struct ClassicMotionSampling {
    double frame{};
    double frames_per_second{24.0};
    std::vector<double> lookup_offsets;
    std::vector<float> placements;
    ClassicAlembicInterpolation interpolation{
        ClassicAlembicInterpolation::Linear};
};

// Sample-invariant data uploaded once and shared by all Classic motion
// dispatches. The deformed RootSample records themselves live per sample.
struct ClassicMotionRootTopology {
    std::vector<std::string> patch_names;
    std::vector<Vec3> reference_positions;
    std::vector<std::uint32_t> primitive_ids;
    std::vector<std::uint32_t> random_prefixes;
    std::vector<std::uint32_t> influence_offsets;
    std::vector<ClassicGuideInfluence> influences;
    std::vector<std::filesystem::path> ptex_maps;
    std::vector<ClassicRootFaceStats> face_stats;
    std::uint64_t candidate_count{};
    std::uint64_t mask_rejected_count{};
    std::uint64_t patch_culled_count{};
    std::uint64_t guide_rejected_count{};
};

struct ClassicCollectionMotionSampleDescription {
    double lookup_offset{};
    float placement{};
    // Samples whose evaluated deformation is bit-identical share one host/GPU
    // preparation. The owning sample points to itself.
    std::size_t deformation_source_index{};
    ClassicAlembicAssetInput surface;
    ClassicDeformedRootPlan roots;
    std::vector<ClassicClumpRuntimeData> clumps;
    std::vector<Vec3> rebuilt_guides;
};

struct ClassicCollectionMotionExecutionDescription {
    std::string name;
    ClassicFloatRuntimePlan runtime;
    ClassicMotionRootTopology root_topology;
    ClassicRuntimeInputData runtime_inputs;
    std::vector<ClassicCollectionMotionSampleDescription> samples;
};

[[nodiscard]] const ClassicCollectionMotionSampleDescription &
resolve_xgen_classic_motion_deformation(
    const ClassicCollectionMotionExecutionDescription &description,
    std::size_t sample);

struct ClassicCollectionMotionExecutionPlan {
    std::filesystem::path collection_path;
    std::filesystem::path archive_path;
    std::filesystem::path descriptions_root;
    ClassicMotionSampling sampling;
    std::size_t context_worker_count{1u};
    std::vector<ClassicCollectionMotionExecutionDescription> descriptions;
};

// Throws std::invalid_argument for malformed RenderAPI motion tables.
void validate_xgen_classic_motion_sampling(
    const ClassicMotionSampling &sampling);

// Resolve either an explicit collection-data root or a project root to the
// directory whose immediate children are Classic descriptions. Relocated
// xgDataPath/xgProjectPath pairs and mixed Windows/Unix separators are
// supported without a recursive filesystem scan.
[[nodiscard]] std::filesystem::path
resolve_xgen_classic_descriptions_root(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &root_or_project);

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

[[nodiscard]] ClassicCollectionMotionExecutionPlan
build_xgen_classic_collection_motion_execution_plan(
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicMotionSampling &sampling,
    const ClassicCollectionExecutionOptions &options = {});

[[nodiscard]] ClassicCollectionMotionExecutionPlan
build_xgen_classic_collection_motion_execution_plan(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicMotionSampling &sampling,
    const ClassicCollectionExecutionOptions &options = {});

} // namespace nanoxgen
