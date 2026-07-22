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
BUILD_DIR="${NANOXGEN_AUTODESK_BUILD_DIR:-${ROOT}/build/autodesk-bridge-release}"
OUTPUT_DIR="${NANOXGEN_MAYA_CACHE_OUTPUT:-${ROOT}/build/xgen-maya-cache}"
STRANDS="${NANOXGEN_MAYA_CACHE_STRANDS:-10000}"
PLUGIN="${BUILD_DIR}/nanoxgen_maya_xgen_cache.so"

"${ROOT}/scripts/check_xgen_sdk.sh" "${XGEN_ROOT}"
mkdir -p "${OUTPUT_DIR}" "${OUTPUT_DIR}/maya-app" "${OUTPUT_DIR}/tmp"
"${CMAKE:-cmake}" --preset release
"${CMAKE:-cmake}" --build --preset release --target nanoxgen_xgen_cache
"${CMAKE:-cmake}" --preset autodesk-bridge-release \
  -DXGEN_ROOT="${XGEN_ROOT}" \
  -DMAYA_LOCATION="${MAYA_LOCATION}" \
  -DNANOXGEN_COMPAT_LIB_DIR="${COMPAT_LIB_DIR}"
"${CMAKE:-cmake}" --build --preset autodesk-bridge-release --target \
  nanoxgen_maya_xgen_cache nanoxgen_tests
"${CTEST:-ctest}" --preset autodesk-bridge-release

RUNTIME_LIBS="${XGEN_ROOT}/lib:${MAYA_LOCATION}/lib"
if [[ -n "${COMPAT_LIB_DIR}" ]]; then
  RUNTIME_LIBS="${COMPAT_LIB_DIR}:${RUNTIME_LIBS}"
fi
PLUGIN_LDD="$(env LD_LIBRARY_PATH="${RUNTIME_LIBS}" ldd "${PLUGIN}")"
if grep -q 'not found' <<<"${PLUGIN_LDD}"; then
  echo "${PLUGIN_LDD}" >&2
  echo "Interactive Maya bridge has unresolved runtime libraries" >&2
  exit 1
fi
if grep -q 'libAdskXGen' <<<"${PLUGIN_LDD}"; then
  echo "Interactive Maya bridge must not link libAdskXGen" >&2
  exit 1
fi

env \
  TMPDIR="${OUTPUT_DIR}/tmp" \
  MAYA_APP_DIR="${OUTPUT_DIR}/maya-app" \
  MAYA_LOCATION="${MAYA_LOCATION}" \
  XGEN_ROOT="${XGEN_ROOT}" \
  MAYA_PLUG_IN_PATH="${XGEN_ROOT}/plug-ins${MAYA_PLUG_IN_PATH:+:${MAYA_PLUG_IN_PATH}}" \
  PYTHONPATH="${MAYA_LOCATION}/scripts:${XGEN_ROOT}/scripts${PYTHONPATH:+:${PYTHONPATH}}" \
  MAYA_DISABLE_CIP=1 \
  MAYA_DISABLE_CER=1 \
  QT_QPA_PLATFORM=offscreen \
  LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${MAYA_LOCATION}/bin/mayapy" \
    "${ROOT}/tests/maya/test_igs_cache_bridge.py" \
    --plugin "${PLUGIN}" \
    --core-cache "${ROOT}/build/release/nanoxgen_xgen_cache" \
    --output-dir "${OUTPUT_DIR}" \
    --strands "${STRANDS}"

echo "Interactive in-memory cache bridge passed; report: ${OUTPUT_DIR}/summary.json"
