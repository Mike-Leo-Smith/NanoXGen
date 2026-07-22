#pragma once

#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic_roots.h"

#include <luisa/dsl/syntax.h>

namespace nanoxgen::luisa_backend {

using PackedGenerateFromRootsKernel = luisa::compute::Kernel1D<void(
    luisa::compute::ByteBuffer,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::float4>)>;

using ClassicBaseGenerateKernel = luisa::compute::Kernel1D<void(
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::uint>,
    luisa::compute::ByteBuffer,
    luisa::compute::Buffer<luisa::float3>,
    luisa::compute::Buffer<luisa::float4>)>;

// Record a backend-neutral fixed-CV guide interpolation kernel. The asset and
// explicit roots are ordinary Luisa buffers, so HIP, Vulkan, and fallback use
// the same DSL source and no CUDA/HIP native handle is exposed by NanoXGen.
// Authored Classic FX are applied by the separate float runtime kernels.
[[nodiscard]] PackedGenerateFromRootsKernel
make_packed_generate_from_roots_kernel(
    const Asset &asset,
    const GenerationParams &params,
    float radius_scale = 1.0f);

// Generate exact-root Classic base curves directly into the final packed
// float4 renderer buffer. Guide rebuilds are uploaded as float3; CSR guide
// associations are traversed in-kernel without per-curve allocations.
[[nodiscard]] ClassicBaseGenerateKernel make_classic_base_generate_kernel(
    std::uint32_t cvs_per_strand,
    float diameter = 0.0f,
    float radius_scale = 1.0f);

} // namespace nanoxgen::luisa_backend
