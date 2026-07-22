#include "nanoxgen/cuda.h"

#include <cuda_runtime.h>

namespace nanoxgen {

namespace {

cudaError_t launch_generate_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    GeneratedOutputView output,
    std::uint32_t block_size,
    cudaStream_t stream);

cudaError_t launch_generate_packed_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    DevicePackedCurveOutputView output,
    std::uint32_t block_size,
    cudaStream_t stream);

} // namespace

__global__ void generate_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    GenerationParams params,
    GeneratedOutputView output) {
    const std::uint32_t strand = blockIdx.x * blockDim.x + threadIdx.x;
    if (strand < params.strand_count) {
        generate_strand(asset, params, strand, output, deformed);
    }
}

__global__ void generate_packed_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    GenerationParams params,
    DevicePackedCurveOutputView output) {
    const std::uint32_t strand = blockIdx.x * blockDim.x + threadIdx.x;
    if (strand < params.strand_count) {
        generate_packed_strand(asset, params, strand, output, deformed);
    }
}

cudaError_t launch_generate_deformed_cuda(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    GeneratedOutputView output,
    cudaStream_t stream) {
    if (!asset || params.strand_count == 0u || params.cvs_per_strand < 2u ||
        (!output.points && !output.widths && !output.roots)) {
        return cudaErrorInvalidValue;
    }
    return launch_generate_kernel(asset, deformed, params, output, 128u, stream);
}

cudaError_t launch_generate_cuda(
    DeviceAssetView asset,
    const GenerationParams &params,
    GeneratedOutputView output,
    cudaStream_t stream) {
    return launch_generate_deformed_cuda(asset, {}, params, output, stream);
}

cudaError_t launch_generate_packed_cuda(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    DevicePackedCurveOutputView output,
    cudaStream_t stream) {
    if (!asset || params.strand_count == 0u || params.cvs_per_strand < 2u ||
        !output.points) {
        return cudaErrorInvalidValue;
    }
    return launch_generate_packed_kernel(
        asset, deformed, params, output, 128u, stream);
}

cudaError_t launch_generate_cuda(
    const DeviceAssetDescriptor &asset,
    const GenerationParams &params,
    const DeviceGeneratedOutputDescriptor &output,
    const DeviceLaunchConfig &config,
    cudaStream_t stream) {
    return launch_generate_deformed_cuda(
        asset, DeviceDeformedGeometryDescriptor{}, params, output, config, stream);
}

cudaError_t launch_generate_deformed_cuda(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DeviceGeneratedOutputDescriptor &output,
    const DeviceLaunchConfig &config,
    cudaStream_t stream) {
    if (validate_device_generation_request(asset, deformed, params, output, config) !=
        DeviceGenerationError::None) {
        return cudaErrorInvalidValue;
    }
    return launch_generate_kernel(
        asset.asset, deformed.geometry, params, output.output, config.block_size, stream);
}

cudaError_t launch_generate_packed_cuda(
    const DeviceAssetDescriptor &asset,
    const DeviceDeformedGeometryDescriptor &deformed,
    const GenerationParams &params,
    const DevicePackedCurveOutputDescriptor &output,
    const DeviceLaunchConfig &config,
    cudaStream_t stream) {
    if (validate_device_packed_generation_request(asset, deformed, params, output, config) !=
        DeviceGenerationError::None) {
        return cudaErrorInvalidValue;
    }
    return launch_generate_packed_kernel(
        asset.asset, deformed.geometry, params, output.output, config.block_size, stream);
}

cudaError_t launch_generate_motion_cuda(
    const DeviceAssetDescriptor &asset,
    const DeviceMotionSampleDescriptor *samples,
    std::uint32_t sample_count,
    const GenerationParams &params,
    const DeviceMotionOutputDescriptor &output,
    const DeviceLaunchConfig &config,
    cudaStream_t stream) {
    if (validate_device_motion_generation_request(
            asset, samples, sample_count, params, output, config) !=
        DeviceGenerationError::None) {
        return cudaErrorInvalidValue;
    }
    const std::uint64_t points_per_sample =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    for (std::uint32_t sample = 0u; sample < sample_count; ++sample) {
        GeneratedOutputView sample_output{};
        sample_output.points = output.points + points_per_sample * sample;
        const cudaError_t error = launch_generate_kernel(
            asset.asset, samples[sample].deformation.geometry, params, sample_output,
            config.block_size, stream);
        if (error != cudaSuccess) { return error; }
    }
    return cudaSuccess;
}

namespace {

cudaError_t launch_generate_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    GeneratedOutputView output,
    std::uint32_t block_size,
    cudaStream_t stream) {
    const std::uint32_t block_count = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(params.strand_count) + block_size - 1u) /
        block_size);
    generate_kernel<<<block_count, block_size, 0u, stream>>>(
        asset, deformed, params, output);
    return cudaGetLastError();
}

cudaError_t launch_generate_packed_kernel(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    DevicePackedCurveOutputView output,
    std::uint32_t block_size,
    cudaStream_t stream) {
    const std::uint32_t block_count = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(params.strand_count) + block_size - 1u) /
        block_size);
    generate_packed_kernel<<<block_count, block_size, 0u, stream>>>(
        asset, deformed, params, output);
    return cudaGetLastError();
}

} // namespace

} // namespace nanoxgen
