#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAYA_LOCATION="${MAYA_LOCATION:-}"
if [[ -z "${MAYA_LOCATION}" ]]; then
  echo "MAYA_LOCATION must point to a complete Maya installation" >&2
  exit 2
fi
XGEN_ROOT="${XGEN_ROOT:-${MAYA_LOCATION}/plug-ins/xgen}"
COMPAT_LIB_DIR="${NANOXGEN_COMPAT_LIB_DIR:-}"
BUILD_DIR="${NANOXGEN_XGEN_BUILD_DIR:-${ROOT}/build/xgen-real}"
REPEATS="${NANOXGEN_BENCHMARK_REPEATS:-7}"
mkdir -p "${BUILD_DIR}" "${BUILD_DIR}/maya-home" "${BUILD_DIR}/maya-app"

RUNTIME_LIBS="${XGEN_ROOT}/lib:${MAYA_LOCATION}/lib"
if [[ -n "${COMPAT_LIB_DIR}" ]]; then
  RUNTIME_LIBS="${RUNTIME_LIBS}:${COMPAT_LIB_DIR}"
fi

make -C "${ROOT}" bin/nanoxgen_benchmark
"${ROOT}/bin/nanoxgen_benchmark" --repeats "${REPEATS}" 10000 100000 \
  > "${BUILD_DIR}/nanoxgen-benchmark.json"

env \
  HOME="${BUILD_DIR}/maya-home" \
  TMPDIR="${TMPDIR:-/tmp}" \
  MAYA_APP_DIR="${BUILD_DIR}/maya-app" \
  MAYA_LOCATION="${MAYA_LOCATION}" \
  XGEN_ROOT="${XGEN_ROOT}" \
  MAYA_PLUG_IN_PATH="${XGEN_ROOT}/plug-ins${MAYA_PLUG_IN_PATH:+:${MAYA_PLUG_IN_PATH}}" \
  PYTHONPATH="${MAYA_LOCATION}/scripts:${XGEN_ROOT}/scripts${PYTHONPATH:+:${PYTHONPATH}}" \
  MAYA_DISABLE_CIP=1 \
  MAYA_DISABLE_CER=1 \
  QT_QPA_PLATFORM=offscreen \
  LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${MAYA_LOCATION}/bin/mayapy" "${ROOT}/tests/maya/benchmark_igs.py" \
    --output "${BUILD_DIR}/xgen-benchmark.json" --repeats "${REPEATS}" 10000 100000

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I"${XGEN_ROOT}/include" "${ROOT}/tools/xgen_benchmark.cpp" \
  -L"${XGEN_ROOT}/lib" -L"${MAYA_LOCATION}/lib" \
  -Wl,--allow-shlib-undefined -lAdskXGen -lclew \
  -o "${BUILD_DIR}/nanoxgen_xgen_benchmark"
LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${BUILD_DIR}/nanoxgen_xgen_benchmark" --repeats "${REPEATS}" \
  "${BUILD_DIR}/xgen-benchmark-10000.xgen" \
  "${BUILD_DIR}/xgen-benchmark-100000.xgen" \
  > "${BUILD_DIR}/xgen-decode-benchmark.json"

echo "NanoXGen: ${BUILD_DIR}/nanoxgen-benchmark.json"
echo "XGen:     ${BUILD_DIR}/xgen-benchmark.json"
echo "XGen API: ${BUILD_DIR}/xgen-decode-benchmark.json"
