#pragma once

#include "nanoxgen/curve_cache.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nanoxgen::classic_typed {

struct Curves {
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
    // Renderer motion samples after sample zero, stored sample-major. Width
    // is sample-invariant and remains in points; these arrays carry xyz only.
    std::vector<std::vector<Vec3>> motion_positions;
    std::vector<Vec2> face_uvs;
    std::vector<std::uint32_t> face_ids;
};

// Position is deliberately generic so the optional Autodesk bridge can pass
// XGenRenderAPI::vec3 directly without copying or aliasing it as a core type.
template<typename Position>
void append_batch(
    std::span<const int> point_counts,
    std::span<const Position> positions,
    std::span<const float> widths,
    std::optional<float> constant_width,
    std::span<const float> u,
    std::span<const float> v,
    std::span<const int> face_ids,
    Curves &output) {
    if (point_counts.empty()) {
        if (!positions.empty() || !widths.empty() || !u.empty() || !v.empty() ||
            !face_ids.empty()) {
            throw std::runtime_error(
                "empty Classic primitive batch has non-empty channels");
        }
        return;
    }
    if (u.size() != point_counts.size() || v.size() != point_counts.size() ||
        face_ids.size() != point_counts.size()) {
        throw std::runtime_error(
            "Classic U/V/FaceID channel sizes do not match primitive topology");
    }

    std::size_t point_total = 0u;
    std::size_t varying_width_total = 0u;
    for (const int count : point_counts) {
        if (count < 2) {
            throw std::runtime_error("Classic curve has fewer than two vertices");
        }
        const std::size_t size = static_cast<std::size_t>(count);
        if (size > std::numeric_limits<std::size_t>::max() - point_total) {
            throw std::runtime_error("Classic curve point count overflows size_t");
        }
        point_total += size;
        varying_width_total += size - 2u;
    }
    if (positions.size() != point_total) {
        throw std::runtime_error(
            "Classic Points size " + std::to_string(positions.size()) +
            " does not match NumVertices total " + std::to_string(point_total));
    }
    const bool varying_widths = !widths.empty() && widths.size() == varying_width_total;
    if (!widths.empty() && widths.size() != point_total && !varying_widths) {
        throw std::runtime_error(
            "Classic Widths size " + std::to_string(widths.size()) +
            " matches neither per-point total " + std::to_string(point_total) +
            " nor B-spline varying total " + std::to_string(varying_width_total));
    }
    if (widths.empty() && !constant_width) {
        throw std::runtime_error("Classic primitive batch has no width channel");
    }
    if (varying_widths) {
        for (const int count : point_counts) {
            if (count == 2) {
                throw std::runtime_error(
                    "two-point Classic curve has no B-spline varying width value");
            }
        }
    }
    if (constant_width &&
        (!std::isfinite(*constant_width) || *constant_width < 0.0f)) {
        throw std::runtime_error("Classic constant width is non-finite or negative");
    }
    for (std::size_t primitive = 0u; primitive < point_counts.size(); ++primitive) {
        if (!std::isfinite(u[primitive]) || !std::isfinite(v[primitive])) {
            throw std::runtime_error("Classic primitive contains non-finite U/V");
        }
        if (face_ids[primitive] < 0) {
            throw std::runtime_error("Classic primitive contains a negative FaceID");
        }
    }
    for (std::size_t point = 0u; point < positions.size(); ++point) {
        const Position &position = positions[point];
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            throw std::runtime_error("Classic primitive contains a non-finite position");
        }
    }
    for (const float width : widths) {
        if (!std::isfinite(width) || width < 0.0f) {
            throw std::runtime_error("Classic primitive contains a non-finite or negative width");
        }
    }
    if (point_counts.size() >
            std::numeric_limits<std::size_t>::max() - output.point_counts.size() ||
        point_total > std::numeric_limits<std::size_t>::max() - output.points.size()) {
        throw std::runtime_error("Classic bridge output size overflows size_t");
    }

    output.point_counts.reserve(output.point_counts.size() + point_counts.size());
    output.face_uvs.reserve(output.face_uvs.size() + point_counts.size());
    output.face_ids.reserve(output.face_ids.size() + point_counts.size());
    output.points.reserve(output.points.size() + point_total);
    for (std::size_t primitive = 0u; primitive < point_counts.size(); ++primitive) {
        output.point_counts.push_back(static_cast<std::uint32_t>(point_counts[primitive]));
        output.face_uvs.push_back({u[primitive], v[primitive]});
        output.face_ids.push_back(static_cast<std::uint32_t>(face_ids[primitive]));
    }
    std::size_t point_offset = 0u;
    std::size_t width_offset = 0u;
    for (const int count_value : point_counts) {
        const std::size_t count = static_cast<std::size_t>(count_value);
        const std::size_t varying_count = count - 2u;
        for (std::size_t cv = 0u; cv < count; ++cv) {
            const Position &position = positions[point_offset + cv];
            float width = widths.empty() ? *constant_width : 0.0f;
            if (!widths.empty()) {
                if (varying_widths) {
                    // XGen's B-spline Points include one duplicated endpoint
                    // on each side. Widths are varying values for the
                    // unduplicated CVs, so expand the two endpoint values while
                    // writing renderer float4s.
                    const std::size_t varying_cv = cv == 0u ? 0u :
                        (cv + 1u == count ? varying_count - 1u : cv - 1u);
                    width = widths[width_offset + varying_cv];
                } else {
                    width = widths[point_offset + cv];
                }
            }
            output.points.push_back(
                {position.x, position.y, position.z, 0.5f * width});
        }
        point_offset += count;
        width_offset += varying_count;
    }
}

template<typename Position>
void append_motion_batch(
    std::span<const int> point_counts,
    std::span<const Position> positions,
    std::size_t primitive_offset,
    std::size_t point_offset,
    std::size_t motion_sample,
    Curves &output) {
    if (motion_sample == 0u ||
        motion_sample > output.motion_positions.size()) {
        throw std::runtime_error(
            "Classic motion sample index is inconsistent");
    }
    if (primitive_offset > output.point_counts.size() ||
        point_counts.size() >
            output.point_counts.size() - primitive_offset) {
        throw std::runtime_error(
            "Classic motion primitive range is inconsistent");
    }
    std::size_t point_total{};
    for (std::size_t primitive = 0u;
         primitive < point_counts.size(); ++primitive) {
        const int count = point_counts[primitive];
        if (count < 2 ||
            static_cast<std::uint32_t>(count) !=
                output.point_counts[primitive_offset + primitive]) {
            throw std::runtime_error(
                "Classic motion NumVertices topology changes across samples");
        }
        const std::size_t size = static_cast<std::size_t>(count);
        if (size > std::numeric_limits<std::size_t>::max() - point_total) {
            throw std::runtime_error(
                "Classic motion point count overflows size_t");
        }
        point_total += size;
    }
    if (positions.size() != point_total ||
        point_offset > output.points.size() ||
        point_total > output.points.size() - point_offset) {
        throw std::runtime_error(
            "Classic motion Points topology changes across samples");
    }
    auto &destination = output.motion_positions[motion_sample - 1u];
    if (destination.size() != point_offset) {
        throw std::runtime_error(
            "Classic motion batches arrived in inconsistent order");
    }
    if (point_total >
        std::numeric_limits<std::size_t>::max() - destination.size()) {
        throw std::runtime_error(
            "Classic motion output size overflows size_t");
    }
    destination.reserve(destination.size() + point_total);
    for (const Position &position : positions) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            throw std::runtime_error(
                "Classic motion primitive contains a non-finite position");
        }
        destination.push_back(
            {position.x, position.y, position.z});
    }
}

} // namespace nanoxgen::classic_typed
