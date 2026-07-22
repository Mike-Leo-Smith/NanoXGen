#pragma once

#include "nanoxgen/generate.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace nanoxgen {

// Host-side metadata for a pointer-free asset blob that has already been
// copied to device memory. The header is the validated host mirror; kernels
// only receive asset, never this descriptor.
struct DeviceAssetDescriptor {
    DeviceAssetView asset;
    AssetHeader header{};
    std::uint64_t byte_capacity{};
};

struct DeviceDeformedGeometryDescriptor {
    DeviceDeformedGeometryView geometry;
    std::uint64_t position_count{};
    std::uint64_t normal_count{};
    std::uint64_t guide_cv_count{};
};

struct DeviceGeneratedOutputDescriptor {
    GeneratedOutputView output;
    std::uint64_t point_capacity{};
    std::uint64_t width_capacity{};
    std::uint64_t root_capacity{};
};

struct DevicePackedCurveOutputDescriptor {
    DevicePackedCurveOutputView output;
    std::uint64_t point_capacity{};
    std::uint64_t root_capacity{};
    std::uint64_t root_uv_capacity{};
    std::uint64_t point_count_capacity{};
};

struct DeviceMotionSampleDescriptor {
    DeviceDeformedGeometryDescriptor deformation;
    float time{};
};

// Sample-major absolute positions. Each sample owns strand_count *
// cvs_per_strand consecutive Vec3 values.
struct DeviceMotionOutputDescriptor {
    Vec3 *points{};
    std::uint64_t point_capacity{};
};

struct DeviceLaunchConfig {
    std::uint32_t block_size{128u};
};

enum class DeviceGenerationError : std::uint32_t {
    None,
    NullAsset,
    MisalignedAsset,
    InvalidAssetMetadata,
    AssetCapacityTooSmall,
    InvalidGenerationParameters,
    InvalidBlockSize,
    DeformedPositionCountMismatch,
    DeformedNormalCountMismatch,
    DeformedGuideCvCountMismatch,
    MissingOutput,
    PointCapacityTooSmall,
    WidthCapacityTooSmall,
    RootCapacityTooSmall,
    RootUvCapacityTooSmall,
    PointCountCapacityTooSmall,
    InvalidRadiusScale,
    MissingMotionSamples,
    InvalidMotionTimes,
    MotionPointCapacityTooSmall,
    OutputSizeOverflow,
};

[[nodiscard]] inline const char *device_generation_error_message(
    DeviceGenerationError error) noexcept {
    switch (error) {
        case DeviceGenerationError::None: return "";
        case DeviceGenerationError::NullAsset: return "device asset pointer is null";
        case DeviceGenerationError::MisalignedAsset:
            return "device asset pointer is not 64-byte aligned";
        case DeviceGenerationError::InvalidAssetMetadata: return "device asset metadata is invalid";
        case DeviceGenerationError::AssetCapacityTooSmall: return "device asset allocation is too small";
        case DeviceGenerationError::InvalidGenerationParameters: return "generation parameters are invalid";
        case DeviceGenerationError::InvalidBlockSize: return "GPU block size must be in [1, 1024]";
        case DeviceGenerationError::DeformedPositionCountMismatch:
            return "deformed position count does not match the asset";
        case DeviceGenerationError::DeformedNormalCountMismatch:
            return "deformed normal count does not match the asset";
        case DeviceGenerationError::DeformedGuideCvCountMismatch:
            return "deformed guide-CV count does not match the asset";
        case DeviceGenerationError::MissingOutput: return "no device output was supplied";
        case DeviceGenerationError::PointCapacityTooSmall: return "device point output is too small";
        case DeviceGenerationError::WidthCapacityTooSmall: return "device width output is too small";
        case DeviceGenerationError::RootCapacityTooSmall: return "device root output is too small";
        case DeviceGenerationError::RootUvCapacityTooSmall: return "device root-UV output is too small";
        case DeviceGenerationError::PointCountCapacityTooSmall:
            return "device point-count output is too small";
        case DeviceGenerationError::InvalidRadiusScale:
            return "device curve radius scale must be finite and non-negative";
        case DeviceGenerationError::MissingMotionSamples: return "no motion samples were supplied";
        case DeviceGenerationError::InvalidMotionTimes:
            return "motion sample times must be finite and strictly increasing";
        case DeviceGenerationError::MotionPointCapacityTooSmall:
            return "device motion-point output is too small";
        case DeviceGenerationError::OutputSizeOverflow: return "device output size overflows uint64";
    }
    return "unknown device generation error";
}

namespace detail {

[[nodiscard]] inline bool valid_generation_parameters(const GenerationParams &params) noexcept {
    return params.strand_count != 0u && params.cvs_per_strand >= 2u &&
        std::isfinite(params.guide_support_scale) && params.guide_support_scale >= 0.0f &&
        std::isfinite(params.guide_weight_power) && params.guide_weight_power >= 0.0f &&
        std::isfinite(params.normal_rejection_cos) && params.normal_rejection_cos >= -1.0f &&
        params.normal_rejection_cos <= 1.0f && std::isfinite(params.length_scale) &&
        params.length_scale >= 0.0f && std::isfinite(params.root_width) &&
        params.root_width >= 0.0f && std::isfinite(params.tip_width) &&
        params.tip_width >= 0.0f && std::isfinite(params.noise_amplitude) &&
        params.noise_amplitude >= 0.0f && std::isfinite(params.noise_frequency) &&
        params.noise_frequency >= 0.0f && std::isfinite(params.noise_mask) &&
        params.noise_mask >= 0.0f && params.noise_mask <= 1.0f &&
        std::isfinite(params.noise_correlation) && params.noise_correlation >= 0.0f &&
        params.noise_correlation <= 1.0f && std::isfinite(params.noise_preserve_length) &&
        params.noise_preserve_length >= 0.0f && params.noise_preserve_length <= 1.0f;
}

[[nodiscard]] inline DeviceGenerationError validate_common_device_input(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DeviceLaunchConfig &config) noexcept {
    if (!asset.asset) { return DeviceGenerationError::NullAsset; }
    if (reinterpret_cast<std::uintptr_t>(asset.asset.data()) % alignof(AssetHeader) != 0u) {
        return DeviceGenerationError::MisalignedAsset;
    }
    const AssetHeader &header = asset.header;
    if (header.magic != kMagic || header.version_major != kVersionMajor ||
        header.version_minor != kVersionMinor ||
        header.byte_size < sizeof(AssetHeader) || header.vertex_count == 0u ||
        header.triangle_count == 0u || header.guide_count == 0u ||
        header.guide_cv_count == 0u || header.guide_stencil_size != kGuideStencilSize ||
        header.noise_gradient_count != kNoiseGradientCount ||
        header.noise_gradients_offset < sizeof(AssetHeader) ||
        header.noise_gradients_offset > header.byte_size ||
        static_cast<std::uint64_t>(header.noise_gradient_count) * sizeof(Vec3) >
            header.byte_size - header.noise_gradients_offset) {
        return DeviceGenerationError::InvalidAssetMetadata;
    }
    if (asset.byte_capacity < header.byte_size) {
        return DeviceGenerationError::AssetCapacityTooSmall;
    }
    if (!valid_generation_parameters(params)) {
        return DeviceGenerationError::InvalidGenerationParameters;
    }
    if (config.block_size == 0u || config.block_size > 1024u) {
        return DeviceGenerationError::InvalidBlockSize;
    }
    if ((deformed.geometry.positions == nullptr) != (deformed.position_count == 0u) ||
        (deformed.geometry.positions && deformed.position_count != header.vertex_count)) {
        return DeviceGenerationError::DeformedPositionCountMismatch;
    }
    if ((deformed.geometry.normals == nullptr) != (deformed.normal_count == 0u) ||
        (deformed.geometry.normals && deformed.normal_count != header.vertex_count)) {
        return DeviceGenerationError::DeformedNormalCountMismatch;
    }
    if ((deformed.geometry.guide_cvs == nullptr) != (deformed.guide_cv_count == 0u) ||
        (deformed.geometry.guide_cvs && deformed.guide_cv_count != header.guide_cv_count)) {
        return DeviceGenerationError::DeformedGuideCvCountMismatch;
    }
    return DeviceGenerationError::None;
}

[[nodiscard]] inline bool optional_capacity_valid(
    const void *pointer, std::uint64_t capacity, std::uint64_t required) noexcept {
    return pointer ? capacity >= required : capacity == 0u;
}

} // namespace detail

[[nodiscard]] inline DeviceGenerationError validate_device_generation_request(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DeviceGeneratedOutputDescriptor &output,
    const DeviceLaunchConfig &config = {}) noexcept {
    if (const DeviceGenerationError error =
            detail::validate_common_device_input(asset, deformed, params, config);
        error != DeviceGenerationError::None) {
        return error;
    }
    if (!output.output.points && !output.output.widths && !output.output.roots) {
        return DeviceGenerationError::MissingOutput;
    }
    const std::uint64_t point_count =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    if (!detail::optional_capacity_valid(
            output.output.points, output.point_capacity, point_count)) {
        return DeviceGenerationError::PointCapacityTooSmall;
    }
    if (!detail::optional_capacity_valid(
            output.output.widths, output.width_capacity, point_count)) {
        return DeviceGenerationError::WidthCapacityTooSmall;
    }
    if (!detail::optional_capacity_valid(
            output.output.roots, output.root_capacity, params.strand_count)) {
        return DeviceGenerationError::RootCapacityTooSmall;
    }
    return DeviceGenerationError::None;
}

[[nodiscard]] inline DeviceGenerationError validate_device_packed_generation_request(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DevicePackedCurveOutputDescriptor &output,
    const DeviceLaunchConfig &config = {}) noexcept {
    if (const DeviceGenerationError error =
            detail::validate_common_device_input(asset, deformed, params, config);
        error != DeviceGenerationError::None) {
        return error;
    }
    if (!output.output.points) { return DeviceGenerationError::MissingOutput; }
    if (!std::isfinite(output.output.radius_scale) || output.output.radius_scale < 0.0f) {
        return DeviceGenerationError::InvalidRadiusScale;
    }
    const std::uint64_t point_count =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    if (output.point_capacity < point_count) {
        return DeviceGenerationError::PointCapacityTooSmall;
    }
    if (!detail::optional_capacity_valid(
            output.output.roots, output.root_capacity, params.strand_count)) {
        return DeviceGenerationError::RootCapacityTooSmall;
    }
    if (!detail::optional_capacity_valid(
            output.output.root_uvs, output.root_uv_capacity, params.strand_count)) {
        return DeviceGenerationError::RootUvCapacityTooSmall;
    }
    if (!detail::optional_capacity_valid(
            output.output.point_counts, output.point_count_capacity, params.strand_count)) {
        return DeviceGenerationError::PointCountCapacityTooSmall;
    }
    return DeviceGenerationError::None;
}

[[nodiscard]] inline DeviceGenerationError validate_device_motion_generation_request(
    const DeviceAssetDescriptor &asset,
    const DeviceMotionSampleDescriptor *samples,
    std::uint32_t sample_count,
    const GenerationParams &params,
    const DeviceMotionOutputDescriptor &output,
    const DeviceLaunchConfig &config = {}) noexcept {
    if (!samples || sample_count == 0u) {
        return DeviceGenerationError::MissingMotionSamples;
    }
    float previous_time = -std::numeric_limits<float>::infinity();
    for (std::uint32_t sample = 0u; sample < sample_count; ++sample) {
        if (!std::isfinite(samples[sample].time) || samples[sample].time <= previous_time) {
            return DeviceGenerationError::InvalidMotionTimes;
        }
        previous_time = samples[sample].time;
        if (const DeviceGenerationError error = detail::validate_common_device_input(
                asset, samples[sample].deformation, params, config);
            error != DeviceGenerationError::None) {
            return error;
        }
    }
    const std::uint64_t points_per_sample =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    if (points_per_sample > std::numeric_limits<std::uint64_t>::max() / sample_count) {
        return DeviceGenerationError::OutputSizeOverflow;
    }
    const std::uint64_t required = points_per_sample * sample_count;
    if (!output.points || output.point_capacity < required) {
        return DeviceGenerationError::MotionPointCapacityTooSmall;
    }
    return DeviceGenerationError::None;
}

} // namespace nanoxgen
