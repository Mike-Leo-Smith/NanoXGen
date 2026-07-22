#include "nanoxgen/cuda.h"

#include <cuda_runtime.h>

namespace nanoxgen {

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
    constexpr std::uint32_t block_size = 128u;
    const std::uint32_t block_count = (params.strand_count + block_size - 1u) / block_size;
    generate_kernel<<<block_count, block_size, 0u, stream>>>(asset, deformed, params, output);
    return cudaGetLastError();
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
    constexpr std::uint32_t block_size = 128u;
    const std::uint32_t block_count = (params.strand_count + block_size - 1u) / block_size;
    generate_packed_kernel<<<block_count, block_size, 0u, stream>>>(
        asset, deformed, params, output);
    return cudaGetLastError();
}

} // namespace nanoxgen
