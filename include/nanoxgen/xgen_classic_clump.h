#pragma once

#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <filesystem>

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

} // namespace nanoxgen
