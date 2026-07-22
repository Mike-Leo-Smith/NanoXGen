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
    ClassicFloatRuntimeExpression noise;
    ClassicFloatRuntimeExpression noise_scale;
    ClassicFloatRuntimeExpression noise_frequency;
    ClassicFloatRuntimeExpression noise_correlation;
    // The point/ID maps consumed by the runtime already encode the authored
    // control-map grouping. Retain the flag for diagnostics and cache keys.
    bool use_control_maps{};
};

// Geometry binding for one compiled ClumpingFX module. Axes are stored
// guide-major with exactly cvs_per_guide float3 values per guide. A strand
// index of kInvalidIndex means the point-map selected no clump guide.
struct ClassicClumpRuntimeData {
    std::string module_name;
    std::uint32_t cvs_per_guide{};
    std::vector<Vec3> guide_axes;
    std::vector<Vec3> guide_normals;
    std::vector<Vec3> guide_tangents;
    std::vector<Vec2> guide_uvs;
    std::vector<std::uint32_t> guide_face_ids;
    std::vector<std::uint32_t> guide_random_prefixes;
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

struct ClassicFloatCustomInput {
    std::string name;
    XgenFloatExpressionProgram program;
};

struct ClassicFloatPrefNoiseInput {
    float frequency{};
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
    // PTEX paths referenced by map() calls, in the stable input order used by
    // every compiled expression. The core does not open these files: the
    // optional native layer samples them once at strand roots and supplies a
    // dense float table to CPU/GPU execution.
    std::vector<std::string> ptex_paths;
    // Palette custom_float_NAME expressions referenced as NAME(). They are
    // evaluated once per root by the optional native input stage, then become
    // ordinary float inputs in both CPU and Luisa execution.
    std::vector<ClassicFloatCustomInput> custom_inputs;
    // Scalar noise($Prefg * frequency) calls. $Prefg is a reference-pose
    // vector, so the optional native stage evaluates these once per root and
    // supplies only the resulting float to retained CPU/GPU execution.
    std::vector<ClassicFloatPrefNoiseInput> pref_noise_inputs;
    // Requirements outside this plan (for example a custom expression or an
    // authored FX module) are preserved as explicit fallback diagnostics.
    // Classic random root sampling is planned separately by xgen_classic_roots.
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
    std::span<const float> ptex_values;
    std::span<const float> custom_values;
    std::span<const float> pref_noise_values;
};

[[nodiscard]] ClassicFloatRuntimePlan compile_xgen_classic_float_runtime_plan(
    const ClassicDescription &description,
    std::span<const ClassicAttribute> palette_attributes = {});

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
    std::span<const ClassicClumpRuntimeData> clump_data = {},
    // Row-major [strand][PTEX, custom, $Prefg-noise inputs]. Kept as plain
    // float data so the same table can be uploaded to GPU backends.
    std::span<const float> runtime_inputs = {});

// Match the Classic renderer cache convention by adding one extrapolated
// endpoint before and after every fixed-CV spline. Call after all FX modules
// and culling have completed.
void add_xgen_classic_renderer_endpoints(PackedGeneratedCurves &curves);

} // namespace nanoxgen
