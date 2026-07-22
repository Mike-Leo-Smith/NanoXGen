#include "nanoxgen/curve_payload.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace nanoxgen {
namespace {

struct ValidatedView {
    CurveSetView source;
    std::vector<std::size_t> offsets;
};

std::size_t checked_add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        throw std::invalid_argument("curve point count overflows size_t");
    }
    return a + b;
}

ValidatedView validate_view(CurveSetView view, const char *label) {
    if (view.strand_count == 0u) {
        throw std::invalid_argument(std::string{label} + " has no strands");
    }
    const bool variable = !view.point_counts.empty();
    if (variable) {
        if (view.point_counts.size() != view.strand_count) {
            throw std::invalid_argument(std::string{label} + " point-count size mismatch");
        }
    } else if (view.fixed_cvs_per_strand < 2u) {
        throw std::invalid_argument(std::string{label} + " needs fixed or variable CV counts");
    }

    ValidatedView result{view, std::vector<std::size_t>(view.strand_count + 1u)};
    for (std::uint32_t strand = 0u; strand < view.strand_count; ++strand) {
        const std::uint32_t count = variable ? view.point_counts[strand]
                                             : view.fixed_cvs_per_strand;
        if (count < 2u) {
            throw std::invalid_argument(std::string{label} + " curve has fewer than two CVs");
        }
        result.offsets[strand + 1u] = checked_add(result.offsets[strand], count);
    }
    if (view.points.size() != result.offsets.back()) {
        throw std::invalid_argument(std::string{label} + " point array size mismatch");
    }
    if (!view.widths.empty() && view.widths.size() != view.points.size()) {
        throw std::invalid_argument(std::string{label} + " width array size mismatch");
    }
    if (view.widths.empty() && (!std::isfinite(view.constant_width) || view.constant_width < 0.0f)) {
        throw std::invalid_argument(std::string{label} + " has an invalid constant width");
    }
    if (!view.roots.empty() && view.roots.size() != view.strand_count) {
        throw std::invalid_argument(std::string{label} + " root array size mismatch");
    }
    if (!view.root_uvs.empty() && view.root_uvs.size() != view.strand_count) {
        throw std::invalid_argument(std::string{label} + " root-UV array size mismatch");
    }
    return result;
}

std::uint32_t curve_count(const ValidatedView &view, std::uint32_t strand) {
    return static_cast<std::uint32_t>(view.offsets[strand + 1u] - view.offsets[strand]);
}

Vec3 lerp(Vec3 a, Vec3 b, float f) noexcept { return a * (1.0f - f) + b * f; }

Vec3 sample_point(const ValidatedView &view, std::uint32_t strand, float t) {
    const std::uint32_t count = curve_count(view, strand);
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(count - 1u);
    const std::uint32_t first = static_cast<std::uint32_t>(x);
    if (first >= count - 1u) { return view.source.points[view.offsets[strand] + count - 1u]; }
    const float f = x - static_cast<float>(first);
    const std::size_t offset = view.offsets[strand] + first;
    return lerp(view.source.points[offset], view.source.points[offset + 1u], f);
}

float sample_width(const ValidatedView &view, std::uint32_t strand, float t) {
    if (view.source.widths.empty()) { return view.source.constant_width; }
    const std::uint32_t count = curve_count(view, strand);
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(count - 1u);
    const std::uint32_t first = static_cast<std::uint32_t>(x);
    if (first >= count - 1u) { return view.source.widths[view.offsets[strand] + count - 1u]; }
    const float f = x - static_cast<float>(first);
    const std::size_t offset = view.offsets[strand] + first;
    return view.source.widths[offset] * (1.0f - f) + view.source.widths[offset + 1u] * f;
}

Vec2 root_uv(const CurveSetView &view, std::uint32_t strand) noexcept {
    if (!view.root_uvs.empty()) { return view.root_uvs[strand]; }
    if (!view.roots.empty()) { return view.roots[strand].uv; }
    return {};
}

bool is_identity(const AffineTransform &transform) noexcept {
    constexpr std::array<float, 12u> identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
    };
    return transform.values == identity;
}

template<typename Primvar>
void validate_primvars(std::span<const Primvar> primvars, std::uint32_t strand_count,
                       const char *label) {
    std::unordered_set<std::string_view> names;
    for (const Primvar &primvar : primvars) {
        if (primvar.name.empty()) {
            throw std::invalid_argument(std::string{label} + " primvar has an empty name");
        }
        if (!names.emplace(primvar.name).second) {
            throw std::invalid_argument(std::string{label} + " primvar name is duplicated");
        }
        if (primvar.values.size() != strand_count) {
            throw std::invalid_argument(std::string{label} + " primvar size mismatch");
        }
    }
}

} // namespace

Vec3 AffineTransform::apply_point(Vec3 p) const noexcept {
    return {
        values[0] * p.x + values[1] * p.y + values[2] * p.z + values[3],
        values[4] * p.x + values[5] * p.y + values[6] * p.z + values[7],
        values[8] * p.x + values[9] * p.y + values[10] * p.z + values[11],
    };
}

CurveSetView view_generated_curves(const GeneratedCurves &curves) noexcept {
    return {
        curves.strand_count,
        curves.cvs_per_strand,
        {},
        curves.points,
        curves.widths,
        curves.roots,
        {},
        0.0f,
    };
}

std::vector<CurveBatch> build_curve_batches(
    CurveSetView base,
    std::span<const MotionSampleView> motion_samples,
    std::span<const UniformFloatPrimvarView> uniform_floats,
    std::span<const UniformColorPrimvarView> uniform_colors,
    const CurvePayloadOptions &options) {
    if (options.max_strands_per_batch == 0u) {
        throw std::invalid_argument("curve batches need a positive strand limit");
    }
    if (options.resample_cv_count == 1u) {
        throw std::invalid_argument("curve resampling needs at least two CVs");
    }
    if (!std::isfinite(options.radius_scale) || options.radius_scale < 0.0f) {
        throw std::invalid_argument("curve radius scale must be finite and non-negative");
    }

    const ValidatedView validated_base = validate_view(base, "base curves");
    std::vector<ValidatedView> validated_motion;
    validated_motion.reserve(motion_samples.size());
    for (const MotionSampleView &sample : motion_samples) {
        if (!std::isfinite(sample.time)) {
            throw std::invalid_argument("motion sample time must be finite");
        }
        if (sample.curves.strand_count != base.strand_count) {
            throw std::invalid_argument("motion sample strand-count mismatch");
        }
        validated_motion.push_back(validate_view(sample.curves, "motion curves"));
    }
    validate_primvars(uniform_floats, base.strand_count, "float");
    validate_primvars(uniform_colors, base.strand_count, "color");
    std::unordered_set<std::string_view> all_primvar_names;
    for (const UniformFloatPrimvarView &primvar : uniform_floats) {
        all_primvar_names.emplace(primvar.name);
    }
    for (const UniformColorPrimvarView &primvar : uniform_colors) {
        if (!all_primvar_names.emplace(primvar.name).second) {
            throw std::invalid_argument("primvar name is shared by multiple data types");
        }
    }
    for (float value : options.transform.values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("curve transform must contain finite values");
        }
    }
    const bool identity_transform = is_identity(options.transform);

    const std::uint32_t batch_count = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(base.strand_count) + options.max_strands_per_batch - 1u) /
        options.max_strands_per_batch);
    std::vector<CurveBatch> batches;
    batches.reserve(batch_count);
    for (std::uint32_t batch_index = 0u; batch_index < batch_count; ++batch_index) {
        CurveBatch batch{};
        batch.first_strand = batch_index * options.max_strands_per_batch;
        const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            base.strand_count,
            static_cast<std::uint64_t>(batch.first_strand) + options.max_strands_per_batch));
        const std::uint32_t strands = end - batch.first_strand;
        batch.point_counts.reserve(strands);
        const bool has_root_uvs = !base.root_uvs.empty() || !base.roots.empty();
        if (has_root_uvs) { batch.root_uvs.reserve(strands); }

        std::size_t batch_points = 0u;
        for (std::uint32_t strand = batch.first_strand; strand < end; ++strand) {
            const std::uint32_t count = options.resample_cv_count != 0u
                ? options.resample_cv_count : curve_count(validated_base, strand);
            batch.point_counts.push_back(count);
            if (has_root_uvs) { batch.root_uvs.push_back(root_uv(base, strand)); }
            batch_points = checked_add(batch_points, count);
        }
        if (options.resample_cv_count == 0u) {
            batch.points.resize(batch_points);
            const std::size_t source_begin = validated_base.offsets[batch.first_strand];
            const Vec3 *source_points = base.points.data() + source_begin;
            const float radius_factor = 0.5f * options.radius_scale;
            if (identity_transform && !base.widths.empty()) {
                const float *source_widths = base.widths.data() + source_begin;
                for (std::size_t point_index = 0u; point_index < batch_points; ++point_index) {
                    const Vec3 point = source_points[point_index];
                    batch.points[point_index] = {
                        point.x, point.y, point.z, source_widths[point_index] * radius_factor};
                }
            } else if (identity_transform) {
                const float radius = base.constant_width * radius_factor;
                for (std::size_t point_index = 0u; point_index < batch_points; ++point_index) {
                    const Vec3 point = source_points[point_index];
                    batch.points[point_index] = {point.x, point.y, point.z, radius};
                }
            } else if (!base.widths.empty()) {
                const float *source_widths = base.widths.data() + source_begin;
                for (std::size_t point_index = 0u; point_index < batch_points; ++point_index) {
                    const Vec3 point = options.transform.apply_point(source_points[point_index]);
                    batch.points[point_index] = {
                        point.x, point.y, point.z, source_widths[point_index] * radius_factor};
                }
            } else {
                const float radius = base.constant_width * radius_factor;
                for (std::size_t point_index = 0u; point_index < batch_points; ++point_index) {
                    const Vec3 point = options.transform.apply_point(source_points[point_index]);
                    batch.points[point_index] = {point.x, point.y, point.z, radius};
                }
            }
        } else {
            batch.points.reserve(batch_points);
            for (std::uint32_t strand = batch.first_strand; strand < end; ++strand) {
                const std::uint32_t count = batch.point_counts[strand - batch.first_strand];
                for (std::uint32_t cv = 0u; cv < count; ++cv) {
                    const float t = static_cast<float>(cv) / static_cast<float>(count - 1u);
                    Vec3 point = sample_point(validated_base, strand, t);
                    const float width = sample_width(validated_base, strand, t);
                    if (!identity_transform) { point = options.transform.apply_point(point); }
                    batch.points.push_back({point.x, point.y, point.z,
                                            0.5f * width * options.radius_scale});
                }
            }
        }

        batch.motion_times.reserve(motion_samples.size());
        batch.motion_points.reserve(batch_points * motion_samples.size());
        for (std::size_t sample_index = 0u; sample_index < motion_samples.size(); ++sample_index) {
            batch.motion_times.push_back(motion_samples[sample_index].time);
            const ValidatedView &sample = validated_motion[sample_index];
            for (std::uint32_t strand = batch.first_strand; strand < end; ++strand) {
                const std::uint32_t count = batch.point_counts[strand - batch.first_strand];
                for (std::uint32_t cv = 0u; cv < count; ++cv) {
                    Vec3 point{};
                    if (options.resample_cv_count == 0u &&
                        curve_count(sample, strand) == count) {
                        point = sample.source.points[sample.offsets[strand] + cv];
                    } else {
                        const float t = static_cast<float>(cv) / static_cast<float>(count - 1u);
                        point = sample_point(sample, strand, t);
                    }
                    if (!identity_transform) { point = options.transform.apply_point(point); }
                    batch.motion_points.push_back(point);
                }
            }
        }

        batch.uniform_floats.reserve(uniform_floats.size());
        for (const UniformFloatPrimvarView &source : uniform_floats) {
            OwnedFloatPrimvar target{std::string{source.name}, {}};
            target.values.assign(source.values.begin() + batch.first_strand,
                                 source.values.begin() + end);
            batch.uniform_floats.emplace_back(std::move(target));
        }
        batch.uniform_colors.reserve(uniform_colors.size());
        for (const UniformColorPrimvarView &source : uniform_colors) {
            OwnedColorPrimvar target{std::string{source.name}, {}};
            target.values.assign(source.values.begin() + batch.first_strand,
                                 source.values.begin() + end);
            batch.uniform_colors.emplace_back(std::move(target));
        }
        batches.emplace_back(std::move(batch));
    }
    return batches;
}

} // namespace nanoxgen
