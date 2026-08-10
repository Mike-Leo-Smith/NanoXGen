#pragma once

#include "nanoxgen/context.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen {

struct ClassicRuntimeInputData {
    std::uint32_t strand_count{};
    std::uint32_t values_per_strand{};
    std::vector<float> values;
};

struct ClassicUniformColorPrimvar {
    std::string name;
    std::vector<Vec3> values;
};

struct ClassicUniformFloatPrimvar {
    std::string name;
    std::vector<float> values;
};

using ClassicUniformColorExpressionEvaluator = void (*)(
    std::string_view source, std::string_view attribute_name,
    std::span<const ClassicAttribute> palette_attributes,
    const ClassicRootPlan &roots, std::span<Vec3> values,
    NanoXGenContext *context);

// Validate the complete RendermanRenderer custom-parameter declaration set.
// NanoXGen must never return a partial curve after silently ignoring a custom
// type, array, or duplicate name it cannot represent.
void validate_xgen_classic_uniform_primvar_declarations(
    const ClassicDescription &description);

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

// Evaluate every custom_float_* attribute as one uniform scalar per retained
// root. Unsupported variables or expressions throw so renderer bridges can
// fall back to native XGen without silently dropping an attribute.
[[nodiscard]] std::vector<ClassicUniformFloatPrimvar>
build_xgen_classic_uniform_float_primvars(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes,
    std::uint32_t description_id,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    NanoXGenContext *context = nullptr,
    float frame = 0.0f);

// Evaluate every RendermanRenderer custom_color_* attribute as one uniform RGB
// value per retained root. Direct PTEX maps are handled internally. Callers
// may provide the embedded typed color-expression evaluator after resolving
// palette function aliases; unsupported expressions throw instead of being
// silently omitted.
[[nodiscard]] std::vector<ClassicUniformColorPrimvar>
build_xgen_classic_uniform_color_primvars(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes,
    const std::filesystem::path &description_directory,
    std::string_view patch_name,
    const ClassicRootPlan &roots,
    ClassicUniformColorExpressionEvaluator expression_evaluator = nullptr,
    NanoXGenContext *context = nullptr);

} // namespace nanoxgen
