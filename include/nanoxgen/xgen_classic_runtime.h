#pragma once

#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_expression.h"

#include <cstdint>
#include <optional>
#include <span>
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

struct ClassicFloatNoiseModule {
    ClassicFloatRuntimeExpression mask;
    ClassicFloatRuntimeExpression magnitude;
    ClassicFloatRuntimeExpression magnitude_scale;
    ClassicFloatRuntimeExpression frequency;
    ClassicFloatRuntimeExpression correlation;
    ClassicFloatRuntimeExpression preserve_length;
    std::uint32_t mode{};
};

struct ClassicFloatClumpModule {
    std::string name;
    ClassicFloatRuntimeExpression mask;
    ClassicFloatRuntimeExpression clump;
    ClassicFloatRuntimeExpression clump_scale;
};

// Geometry binding for one compiled ClumpingFX module. Axes are stored
// guide-major with exactly cvs_per_guide float3 values per guide. A strand
// index of kInvalidIndex means the point-map selected no clump guide.
struct ClassicClumpRuntimeData {
    std::string module_name;
    std::uint32_t cvs_per_guide{};
    std::vector<Vec3> guide_axes;
    std::vector<std::uint32_t> strand_guide_indices;
};

enum class ClassicFloatEffectType : std::uint8_t {
    Clump,
    Noise,
    Cut,
};

struct ClassicFloatEffect {
    ClassicFloatEffectType type{};
    std::uint32_t module_index{};
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
    std::vector<ClassicFloatNoiseModule> noises;
    std::vector<ClassicFloatClumpModule> clumps;
    std::vector<ClassicFloatCutModule> cuts;
    std::vector<ClassicFloatEffect> effects;
    // Requirements outside this plan (for example PTEX or an authored FX
    // module) are preserved as explicit fallback diagnostics. Classic random
    // root sampling is planned separately by xgen_classic_roots.
    std::vector<std::string> fallback_reasons;

    [[nodiscard]] bool lowering_complete() const noexcept {
        return fallback_reasons.empty();
    }
};

// Float implementation of the SeExpr 3D noise used by Classic NoiseFX.
[[nodiscard]] float xgen_classic_noise_float(Vec3 sample) noexcept;

struct ClassicFloatRuntimeContext {
    std::uint32_t id{};
    float u{};
    float v{};
    float face_seed{};
    float c_length{};
    float c_width{};
    float t{};
    std::uint32_t random_prefix{};
    bool has_random_prefix{};
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
    float radius_scale = 1.0f,
    std::span<const Vec3> surface_tangents = {},
    std::span<const std::uint32_t> random_prefixes = {},
    std::span<const std::uint32_t> primitive_ids = {},
    std::span<const ClassicClumpRuntimeData> clump_data = {});

// Match the Classic renderer cache convention by adding one extrapolated
// endpoint before and after every fixed-CV spline. Call after all FX modules
// and culling have completed.
void add_xgen_classic_renderer_endpoints(PackedGeneratedCurves &curves);

} // namespace nanoxgen
