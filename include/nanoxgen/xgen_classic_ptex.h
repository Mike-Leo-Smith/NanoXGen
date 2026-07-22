#pragma once

#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace nanoxgen {

struct ClassicPtexRuntimeData {
    std::uint32_t strand_count{};
    std::uint32_t values_per_strand{};
    std::vector<float> values;
};

// Resolve ${DESC}, open each map retained by the runtime plan, and point-sample
// it at every stable root identity. This is an optional host preprocessing
// stage; the retained runtime and all GPU kernels consume only the float table.
[[nodiscard]] ClassicPtexRuntimeData build_xgen_classic_ptex_runtime_data(
    const ClassicFloatRuntimePlan &plan,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots);

} // namespace nanoxgen
