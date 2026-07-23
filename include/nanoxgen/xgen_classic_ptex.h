#pragma once

#include "nanoxgen/context.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace nanoxgen {

struct ClassicRuntimeInputData {
    std::uint32_t strand_count{};
    std::uint32_t values_per_strand{};
    std::vector<float> values;
};

// Resolve and point-sample each retained PTEX map, evaluate retained palette
// scalar functions, and bind supported $Prefg vector noise at every stable
// root identity. This is an optional host preprocessing stage; the retained
// runtime and all GPU kernels consume only the resulting float table.
[[nodiscard]] ClassicRuntimeInputData build_xgen_classic_runtime_input_data(
    const ClassicFloatRuntimePlan &plan,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    NanoXGenContext *context = nullptr);

} // namespace nanoxgen
