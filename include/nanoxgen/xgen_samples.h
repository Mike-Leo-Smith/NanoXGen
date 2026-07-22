#pragma once

#include "nanoxgen/types.h"

#include <cstdint>

namespace nanoxgen {

struct XgenSample {
    double x{};
    double y{};
};

// Maya 2027 Classic RandomGenerator uses sixteen fixed 32768-sample patterns,
// extends them in progressively finer square tiles, and applies one of eight
// square symmetries selected by descriptionId. The embedded table retains the
// exact host-side double bit patterns needed for Autodesk-compatible density
// and mask decisions. This wrapper narrows only the final sample to the
// float-only RootSample/GPU ABI.
[[nodiscard]] Vec2 xgen_random_sample(
    std::uint32_t index, std::uint32_t description_id,
    std::uint32_t group) noexcept;

// Exact host-side form used while reproducing Autodesk density and mask
// decisions. Runtime/device code should use xgen_random_sample(), whose float
// ABI avoids FP64 work on GPUs.
[[nodiscard]] XgenSample xgen_random_sample_exact(
    std::uint32_t index, std::uint32_t description_id,
    std::uint32_t group) noexcept;

inline constexpr std::uint32_t kXgenSampleGroupCount = 16u;
inline constexpr std::uint32_t kXgenSamplesPerGroup = 32768u;
inline constexpr std::uint32_t kXgenMaximumUniqueSample = 622592u;

} // namespace nanoxgen
