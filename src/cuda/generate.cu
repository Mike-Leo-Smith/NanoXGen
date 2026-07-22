#include "nanoxgen/generate.h"

#include <cuda_runtime.h>

namespace nanoxgen {

__global__ void generate_kernel(
    DeviceAssetView asset, GenerationParams params, GeneratedOutputView output) {
    const std::uint32_t strand = blockIdx.x * blockDim.x + threadIdx.x;
    if (strand < params.strand_count) { generate_strand(asset, params, strand, output); }
}

cudaError_t launch_generate_cuda(
    DeviceAssetView asset,
    const GenerationParams &params,
    GeneratedOutputView output,
    cudaStream_t stream) {
    constexpr std::uint32_t block_size = 128u;
    const std::uint32_t block_count = (params.strand_count + block_size - 1u) / block_size;
    generate_kernel<<<block_count, block_size, 0u, stream>>>(asset, params, output);
    return cudaGetLastError();
}

} // namespace nanoxgen
