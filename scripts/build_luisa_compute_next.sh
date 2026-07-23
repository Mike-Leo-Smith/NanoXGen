#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <LuisaCompute-next-source> [build-directory]" >&2
    exit 2
fi

luisa_source=$1
luisa_build=${2:-"${luisa_source}/build-nanoxgen-hip"}
rocm_root=${ROCM_PATH:-/opt/rocm}
llvm_dir=${LLVM_DIR:-/usr/lib/cmake/llvm}
hip_arch=${NANOXGEN_HIP_ARCH:-gfx1201}
enable_fallback=${NANOXGEN_LUISA_ENABLE_FALLBACK:-OFF}

if [[ ! -d "${luisa_source}/.git" || ! -f "${luisa_source}/src/backends/hip/CMakeLists.txt" ]]; then
    echo "error: source must be a recursive LuisaCompute next checkout" >&2
    exit 1
fi

cmake -S "${luisa_source}" -B "${luisa_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${rocm_root}" \
    -DLLVM_DIR="${llvm_dir}" \
    -DCMAKE_HIP_ARCHITECTURES="${hip_arch}" \
    -DHIPRT_GPU_ARCHS="${hip_arch}" \
    -DLUISA_COMPUTE_ENABLE_HIP=ON \
    -DLUISA_COMPUTE_ENABLE_CUDA=OFF \
    -DLUISA_COMPUTE_ENABLE_DX=OFF \
    -DLUISA_COMPUTE_ENABLE_METAL=OFF \
    -DLUISA_COMPUTE_ENABLE_VULKAN=OFF \
    -DLUISA_COMPUTE_ENABLE_CPU=OFF \
    -DLUISA_COMPUTE_ENABLE_FALLBACK="${enable_fallback}" \
    -DLUISA_COMPUTE_ENABLE_REMOTE=OFF \
    -DLUISA_COMPUTE_ENABLE_GUI=OFF \
    -DLUISA_COMPUTE_ENABLE_TENSOR=OFF \
    -DLUISA_COMPUTE_ENABLE_CLANG_CXX=OFF \
    -DLUISA_COMPUTE_BUILD_TESTS=OFF
cmake --build "${luisa_build}" --target luisa-compute-backend-hip \
    -j"${NANOXGEN_BUILD_JOBS:-$(nproc)}"
if [[ "${enable_fallback}" == "ON" ]]; then
    cmake --build "${luisa_build}" --target luisa-compute-backend-fallback \
        -j"${NANOXGEN_BUILD_JOBS:-$(nproc)}"
fi

echo "LuisaCompute source: $(git -C "${luisa_source}" rev-parse HEAD)"
echo "LuisaCompute build:  ${luisa_build}"
echo "Fallback backend:    ${enable_fallback}"
