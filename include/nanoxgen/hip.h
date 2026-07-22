#pragma once

#include "nanoxgen/device_contract.h"

#include <hip/hip_runtime_api.h>

namespace nanoxgen {

// Checked production entry points. These validate the host-side asset mirror,
// deformation lengths, output capacities, numeric parameters, and launch
// geometry before enqueueing work on stream.
[[nodiscard]] hipError_t launch_generate_hip(
    const DeviceAssetDescriptor &asset,
    const GenerationParams &params,
    const DeviceGeneratedOutputDescriptor &output,
    const DeviceLaunchConfig &config = {},
    hipStream_t stream = nullptr);

[[nodiscard]] hipError_t launch_generate_deformed_hip(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DeviceGeneratedOutputDescriptor &output,
    const DeviceLaunchConfig &config = {},
    hipStream_t stream = nullptr);

[[nodiscard]] hipError_t launch_generate_packed_hip(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DevicePackedCurveOutputDescriptor &output,
    const DeviceLaunchConfig &config = {},
    hipStream_t stream = nullptr);

// Validate every shutter sample before enqueueing any work, then write
// sample-major absolute positions in stream order. Root identity is preserved
// because every sample uses the same asset, seed, and generation parameters.
[[nodiscard]] hipError_t launch_generate_motion_hip(
    const DeviceAssetDescriptor &asset,
    const DeviceMotionSampleDescriptor *samples,
    std::uint32_t sample_count,
    const GenerationParams &params,
    const DeviceMotionOutputDescriptor &output,
    const DeviceLaunchConfig &config = {},
    hipStream_t stream = nullptr);

// Low-level compatibility entry points. The caller owns all allocation-size
// guarantees; prefer the descriptor overloads above for renderer integration.
[[nodiscard]] hipError_t launch_generate_hip(
    DeviceAssetView asset,
    const GenerationParams &params,
    GeneratedOutputView output,
    hipStream_t stream = nullptr);

[[nodiscard]] hipError_t launch_generate_deformed_hip(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    GeneratedOutputView output,
    hipStream_t stream = nullptr);

// Direct renderer layout: no intermediate positions/width arrays and no CPU
// packing pass. points and optional root arrays must already reside on device.
[[nodiscard]] hipError_t launch_generate_packed_hip(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    DevicePackedCurveOutputView output,
    hipStream_t stream = nullptr);

} // namespace nanoxgen
