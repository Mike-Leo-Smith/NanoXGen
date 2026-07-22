#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${NANOXGEN_FAST_MATH_BUILD_DIR:-${ROOT}/build/fast-math}"
REPEATS="${NANOXGEN_FAST_MATH_REPEATS:-15}"
CXX="${CXX:-g++}"
SIMD_WIDTH="${NANOXGEN_SIMD_WIDTH:-256}"
mkdir -p "${BUILD_DIR}"

COMMON=(
  -I"${ROOT}/include" -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
  -pthread -march=native -mtune=native -mprefer-vector-width="${SIMD_WIDTH}"
  "${ROOT}/src/asset.cpp" "${ROOT}/src/curve_payload.cpp"
  "${ROOT}/tools/precision.cpp"
)

"${CXX}" "${COMMON[@]}" -o "${BUILD_DIR}/precision-strict"
"${CXX}" "${COMMON[@]}" -ffast-math -o "${BUILD_DIR}/precision-fast"

"${BUILD_DIR}/precision-strict" write "${BUILD_DIR}/strict.bin"
"${BUILD_DIR}/precision-fast" write "${BUILD_DIR}/fast.bin"
"${BUILD_DIR}/precision-strict" check-relaxed \
  "${BUILD_DIR}/strict.bin" "${BUILD_DIR}/fast.bin" \
  > "${BUILD_DIR}/error.json"
"${BUILD_DIR}/precision-strict" benchmark "${REPEATS}" \
  > "${BUILD_DIR}/strict-benchmark.json"
"${BUILD_DIR}/precision-fast" benchmark "${REPEATS}" \
  > "${BUILD_DIR}/fast-benchmark.json"

echo "Error:  ${BUILD_DIR}/error.json"
echo "Strict: ${BUILD_DIR}/strict-benchmark.json"
echo "Fast:   ${BUILD_DIR}/fast-benchmark.json"
