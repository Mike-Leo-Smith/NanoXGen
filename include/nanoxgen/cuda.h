#pragma once

#include "nanoxgen/generate.h"

#include <cuda_runtime_api.h>

namespace nanoxgen {

[[nodiscard]] cudaError_t launch_generate_cuda(
    DeviceAssetView asset,
    const GenerationParams &params,
    GeneratedOutputView output,
    cudaStream_t stream = nullptr);

[[nodiscard]] cudaError_t launch_generate_deformed_cuda(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    GeneratedOutputView output,
    cudaStream_t stream = nullptr);

// Direct renderer layout: no intermediate positions/width arrays and no CPU
// packing pass. points and optional root arrays must already reside on device.
[[nodiscard]] cudaError_t launch_generate_packed_cuda(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    DevicePackedCurveOutputView output,
    cudaStream_t stream = nullptr);

} // namespace nanoxgen
