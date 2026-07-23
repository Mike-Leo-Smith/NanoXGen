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
enable_hip=${NANOXGEN_LUISA_ENABLE_HIP:-ON}
enable_vulkan=${NANOXGEN_LUISA_ENABLE_VULKAN:-OFF}
enable_fallback=${NANOXGEN_LUISA_ENABLE_FALLBACK:-OFF}
if [[ -n "${NANOXGEN_BUILD_JOBS:-}" ]]; then
    build_jobs=${NANOXGEN_BUILD_JOBS}
elif command -v nproc >/dev/null 2>&1; then
    build_jobs=$(nproc)
else
    build_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
fi

if [[ ! -d "${luisa_source}/.git" ||
      ! -f "${luisa_source}/CMakeLists.txt" ]]; then
    echo "error: source must be a recursive LuisaCompute next checkout" >&2
    exit 1
fi
if [[ "${enable_hip}" != "ON" && "${enable_vulkan}" != "ON" &&
      "${enable_fallback}" != "ON" ]]; then
    echo "error: enable at least one of HIP, Vulkan, or fallback" >&2
    exit 1
fi

cmake -S "${luisa_source}" -B "${luisa_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${rocm_root}" \
    -DLLVM_DIR="${llvm_dir}" \
    -DCMAKE_HIP_ARCHITECTURES="${hip_arch}" \
    -DHIPRT_GPU_ARCHS="${hip_arch}" \
    -DLUISA_COMPUTE_ENABLE_HIP="${enable_hip}" \
    -DLUISA_COMPUTE_ENABLE_CUDA=OFF \
    -DLUISA_COMPUTE_ENABLE_DX=OFF \
    -DLUISA_COMPUTE_ENABLE_METAL=OFF \
    -DLUISA_COMPUTE_ENABLE_VULKAN="${enable_vulkan}" \
    -DLUISA_COMPUTE_ENABLE_CPU=OFF \
    -DLUISA_COMPUTE_ENABLE_FALLBACK="${enable_fallback}" \
    -DLUISA_COMPUTE_ENABLE_REMOTE=OFF \
    -DLUISA_COMPUTE_ENABLE_GUI=OFF \
    -DLUISA_COMPUTE_ENABLE_TENSOR=OFF \
    -DLUISA_COMPUTE_ENABLE_CLANG_CXX=OFF \
    -DLUISA_COMPUTE_BUILD_TESTS=OFF

# CMake does not delete a plug-in merely because a backend was disabled on a
# later configure. Remove only those explicitly disabled artifacts so an old
# ABI-compatible-looking filename cannot be loaded beside the new runtime.
if [[ "${enable_hip}" != "ON" ]]; then
    cmake -E rm -f "${luisa_build}/bin/libluisa-backend-hip.so"
fi
if [[ "${enable_vulkan}" != "ON" ]]; then
    cmake -E rm -f "${luisa_build}/bin/libluisa-backend-vk.so"
fi
if [[ "${enable_fallback}" != "ON" ]]; then
    cmake -E rm -f "${luisa_build}/bin/libluisa-backend-fallback.so"
fi

if [[ "${enable_hip}" == "ON" ]]; then
    cmake --build "${luisa_build}" --target luisa-compute-backend-hip \
        -j"${build_jobs}"
fi
if [[ "${enable_vulkan}" == "ON" ]]; then
    cmake --build "${luisa_build}" --target luisa-compute-backend-vk \
        -j"${build_jobs}"
fi
if [[ "${enable_fallback}" == "ON" ]]; then
    cmake --build "${luisa_build}" --target luisa-compute-backend-fallback \
        -j"${build_jobs}"
fi

echo "LuisaCompute source: $(git -C "${luisa_source}" rev-parse HEAD)"
echo "LuisaCompute build:  ${luisa_build}"
echo "HIP backend:         ${enable_hip}"
echo "Vulkan backend:      ${enable_vulkan}"
echo "Fallback backend:    ${enable_fallback}"
