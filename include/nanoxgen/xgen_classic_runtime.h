#pragma once

#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_expression.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nanoxgen {

// A compiled authoring expression and the identity needed by CPU/GPU
// backends to bind its XGen variables. The program itself is float-only.
struct ClassicFloatRuntimeExpression {
    std::string object_type;
    std::string object_name;
    std::string attribute;
    XgenFloatExpressionProgram program;
};

struct ClassicFloatCutModule {
    ClassicFloatRuntimeExpression amount;
    // XGen rebuildType 1 is the reparameterized fixed-CV path implemented by
    // the runtime. Keep-param and unknown modes remain explicit fallbacks.
    std::uint32_t rebuild_type{};
};

// Runtime subset of one Classic SplinePrimitive. Authoring values are parsed
// losslessly elsewhere; this retained/JIT form contains float/uint data only.
struct ClassicFloatRuntimePlan {
    std::string description_name;
    std::uint32_t description_id{};
    std::uint32_t fx_cv_count{};
    std::optional<ClassicFloatRuntimeExpression> length;
    std::optional<ClassicFloatRuntimeExpression> width;
    std::optional<ClassicFloatRuntimeExpression> taper;
    std::optional<ClassicFloatRuntimeExpression> taper_start;
    std::optional<ClassicFloatRuntimeExpression> width_ramp;
    std::vector<ClassicFloatCutModule> cuts;
    // Requirements outside this plan (for example PTEX, root sampling, or an
    // authored FX module) are preserved as explicit fallback diagnostics.
    std::vector<std::string> fallback_reasons;

    [[nodiscard]] bool lowering_complete() const noexcept {
        return fallback_reasons.empty();
    }
};

struct ClassicFloatRuntimeContext {
    std::uint32_t id{};
    float u{};
    float v{};
    float face_seed{};
    float c_length{};
    float c_width{};
    float t{};
};

[[nodiscard]] ClassicFloatRuntimePlan compile_xgen_classic_float_runtime_plan(
    const ClassicDescription &description);

// Bind the supported public XGen scalar variables and evaluate the exact same
// float IR accepted by the LuisaCompute lowering.
[[nodiscard]] float evaluate_xgen_classic_float_runtime_expression(
    const ClassicFloatRuntimeExpression &expression,
    const ClassicFloatRuntimeContext &context);

// Apply length, reparameterized CutFX, base width, taper, and widthRamp to an
// already generated fixed-CV packed curve set. Positions and radii are updated
// in place without converting through double.
void apply_xgen_classic_float_runtime_plan_cpu(
    PackedGeneratedCurves &curves,
    const ClassicFloatRuntimePlan &plan,
    float radius_scale = 1.0f);

} // namespace nanoxgen
