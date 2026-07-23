#pragma once

#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <filesystem>
#include <vector>

namespace nanoxgen {

// Bind one basic ClumpingFX module to its authored XPD point file and PTEX
// ID map. The optional Classic native target owns filesystem/Ptex decoding;
// the resulting float/uint geometry binding is backend-neutral.
[[nodiscard]] ClassicClumpRuntimeData build_xgen_classic_clump_runtime_data(
    const ClassicDescription &description,
    const ClassicAlembicAssetInput &surface,
    const std::filesystem::path &description_directory,
    const ClassicRootPlan &strand_roots,
    const ClassicFloatRuntimePlan &runtime_plan,
    std::size_t module_index,
    std::uint32_t cvs_per_guide);

// Bind independent top-level ClumpingFX modules with a bounded worker pool.
// Results retain authored module order; recursive upstream evaluation inside
// each module remains deterministic.
[[nodiscard]] std::vector<ClassicClumpRuntimeData>
build_xgen_classic_clump_runtime_data_parallel(
    const ClassicDescription &description,
    const ClassicAlembicAssetInput &surface,
    const std::filesystem::path &description_directory,
    const ClassicRootPlan &strand_roots,
    const ClassicFloatRuntimePlan &runtime_plan,
    std::uint32_t cvs_per_guide,
    std::size_t max_workers = 8u);

} // namespace nanoxgen
