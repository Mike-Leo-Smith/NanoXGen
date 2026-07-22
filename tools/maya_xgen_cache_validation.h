#pragma once

#include "nanoxgen/xgen.h"

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <tuple>

namespace nanoxgen::maya_xgen_cache {

inline auto identity(const XGenEvaluatedCurves &curves, std::size_t curve) noexcept {
    return std::tuple{
        curves.face_ids[curve],
        std::bit_cast<std::uint32_t>(curves.face_uvs[curve].x),
        std::bit_cast<std::uint32_t>(curves.face_uvs[curve].y),
        std::bit_cast<std::uint32_t>(curves.patch_uvs[curve].x),
        std::bit_cast<std::uint32_t>(curves.patch_uvs[curve].y)};
}

inline void validate_unique_canonical_identities(
    const XGenEvaluatedCurves &curves) {
    if (curves.face_ids.size() != curves.point_counts.size() ||
        curves.face_uvs.size() != curves.point_counts.size() ||
        curves.patch_uvs.size() != curves.point_counts.size()) {
        throw std::runtime_error("canonical identity channel sizes disagree");
    }
    for (std::size_t curve = 1u; curve < curves.point_counts.size(); ++curve) {
        const auto previous = identity(curves, curve - 1u);
        const auto current = identity(curves, curve);
        if (current < previous) {
            throw std::runtime_error("curve identities are not in canonical order");
        }
        if (current == previous) {
            throw std::runtime_error("duplicate canonical curve identity");
        }
    }
}

} // namespace nanoxgen::maya_xgen_cache
