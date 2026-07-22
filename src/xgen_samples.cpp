#include "nanoxgen/xgen_samples.h"

#include <array>
#include <bit>
#include <cstdint>

namespace nanoxgen {
namespace {

struct PackedSample {
    std::uint64_t x;
    std::uint64_t y;
};

inline constexpr std::array<
    PackedSample, kXgenSampleGroupCount * kXgenSamplesPerGroup>
    kSamplePattern{{
#include "xgen_sample_pattern.inc"
    }};

XgenSample base_sample(std::uint32_t group, std::uint32_t index) noexcept {
    const PackedSample packed = kSamplePattern[
        (group & (kXgenSampleGroupCount - 1u)) * kXgenSamplesPerGroup + index];
    return {std::bit_cast<double>(packed.x), std::bit_cast<double>(packed.y)};
}

} // namespace

XgenSample xgen_random_sample_exact(std::uint32_t index,
                                    std::uint32_t description_id,
                                    std::uint32_t group) noexcept {
    XgenSample sample{};
    if (index < kXgenSamplesPerGroup) {
        sample = base_sample(group, index);
    } else {
        std::uint32_t side = 1u;
        do {
            index -= side * side * kXgenSamplesPerGroup;
            side *= 2u;
        } while (index >= side * side * kXgenSamplesPerGroup);
        const std::uint32_t tile_x = index % side;
        const std::uint32_t tile_y = (index / side) % side;
        const XgenSample base = base_sample(group, index / (side * side));
        const double inverse_side = 1.0 / static_cast<double>(side);
        sample = {(static_cast<double>(tile_x) + base.x) * inverse_side,
                  (static_cast<double>(tile_y) + base.y) * inverse_side};
    }

    const double one_minus_x = 1.0 - sample.x;
    const double one_minus_y = 1.0 - sample.y;
    switch (description_id & 7u) {
    case 0u: return sample;
    case 1u: return {one_minus_y, sample.x};
    case 2u: return {one_minus_x, one_minus_y};
    case 3u: return {sample.y, one_minus_x};
    case 4u: return {one_minus_x, sample.y};
    case 5u: return {sample.y, sample.x};
    case 6u: return {sample.x, one_minus_y};
    default: return {one_minus_y, one_minus_x};
    }
}

Vec2 xgen_random_sample(std::uint32_t index,
                        std::uint32_t description_id,
                        std::uint32_t group) noexcept {
    const XgenSample sample = xgen_random_sample_exact(
        index, description_id, group);
    return {static_cast<float>(sample.x), static_cast<float>(sample.y)};
}

} // namespace nanoxgen
