#pragma once

#include "nanoxgen/asset.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen {

// Row-major affine transform. Width scaling is intentionally independent: a
// non-uniform affine transform has no unique scalar radius transform.
struct AffineTransform {
    std::array<float, 12u> values{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
    };

    [[nodiscard]] Vec3 apply_point(Vec3 p) const noexcept;
};

// A non-owning curve view. point_counts may be omitted when every curve has
// fixed_cvs_per_strand CVs. Widths are full diameters, matching XGen.
struct CurveSetView {
    std::uint32_t strand_count{};
    std::uint32_t fixed_cvs_per_strand{};
    std::span<const std::uint32_t> point_counts;
    std::span<const Vec3> points;
    std::span<const float> widths;
    std::span<const RootSample> roots;
    std::span<const Vec2> root_uvs;
    float constant_width{};
};

[[nodiscard]] CurveSetView view_generated_curves(const GeneratedCurves &curves) noexcept;

struct MotionSampleView {
    float time{};
    CurveSetView curves;
};

struct UniformFloatPrimvarView {
    std::string_view name;
    std::span<const float> values;
};

struct UniformColorPrimvarView {
    std::string_view name;
    std::span<const Vec3> values;
};

struct OwnedFloatPrimvar {
    std::string name;
    std::vector<float> values;
};

struct OwnedColorPrimvar {
    std::string name;
    std::vector<Vec3> values;
};

struct CurveBatch {
    std::uint32_t first_strand{};
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
    std::vector<Vec2> root_uvs;
    std::vector<float> motion_times;
    // Sample-major absolute positions. Each sample contains points.size()
    // entries in the same curve/CV order as points.
    std::vector<Vec3> motion_points;
    std::vector<OwnedFloatPrimvar> uniform_floats;
    std::vector<OwnedColorPrimvar> uniform_colors;
};

struct CurvePayloadOptions {
    std::uint32_t max_strands_per_batch{64u * 1024u};
    // Zero preserves source CV counts. A positive value resamples every curve
    // in normalized index space and must be at least two.
    std::uint32_t resample_cv_count{};
    float radius_scale{1.0f};
    AffineTransform transform{};
};

[[nodiscard]] std::vector<CurveBatch> build_curve_batches(
    CurveSetView base,
    std::span<const MotionSampleView> motion_samples = {},
    std::span<const UniformFloatPrimvarView> uniform_floats = {},
    std::span<const UniformColorPrimvarView> uniform_colors = {},
    const CurvePayloadOptions &options = {});

} // namespace nanoxgen
