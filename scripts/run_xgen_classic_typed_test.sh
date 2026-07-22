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

usage() {
  echo "usage: $0 --xgen-file FILE --palette NAME --geom FILE --patch NAME \
--description NAME [--output FILE] [--frame NUMBER]" >&2
}

XGEN_FILE=""
PALETTE=""
GEOMETRY=""
PATCH=""
DESCRIPTION=""
OUTPUT="${ROOT}/build/xgen-classic-real/classic.nxc"
FRAME="1"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --xgen-file) XGEN_FILE="${2:-}"; shift 2 ;;
    --palette) PALETTE="${2:-}"; shift 2 ;;
    --geom) GEOMETRY="${2:-}"; shift 2 ;;
    --patch) PATCH="${2:-}"; shift 2 ;;
    --description) DESCRIPTION="${2:-}"; shift 2 ;;
    --output) OUTPUT="${2:-}"; shift 2 ;;
    --frame) FRAME="${2:-}"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "${XGEN_FILE}" || -z "${PALETTE}" || -z "${GEOMETRY}" ||
      -z "${PATCH}" || -z "${DESCRIPTION}" ]]; then
  usage
  exit 2
fi
if [[ ! -f "${XGEN_FILE}" || ! -f "${GEOMETRY}" ]]; then
  echo "Classic collection or geometry file does not exist" >&2
  exit 2
fi

"${ROOT}/scripts/check_xgen_sdk.sh" "${XGEN_ROOT}"
mkdir -p "$(dirname "${OUTPUT}")"
"${CMAKE:-cmake}" --preset autodesk-bridge-release \
  -DXGEN_ROOT="${XGEN_ROOT}" \
  -DNANOXGEN_COMPAT_LIB_DIR="${COMPAT_LIB_DIR}"
"${CMAKE:-cmake}" --build --preset autodesk-bridge-release --target \
  nanoxgen_xgen_classic_typed nanoxgen_tests
"${CTEST:-ctest}" --preset autodesk-bridge-release

RUNTIME_LIBS="${XGEN_ROOT}/lib:${MAYA_LOCATION}/lib"
if [[ -n "${COMPAT_LIB_DIR}" ]]; then
  RUNTIME_LIBS="${COMPAT_LIB_DIR}:${RUNTIME_LIBS}"
fi
BRIDGE="${BUILD_DIR}/nanoxgen_xgen_classic_typed"
if env LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    ldd "${BRIDGE}" | grep -q 'not found'; then
  env LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    ldd "${BRIDGE}" >&2
  echo "Classic bridge has unresolved runtime libraries" >&2
  exit 1
fi

RENDER_ARGS="-debug 0 -warning 1 -stats 0 -frame ${FRAME} -shutter 0.0 \
-file ${XGEN_FILE} -palette ${PALETTE} -geom ${GEOMETRY} -patch ${PATCH} \
-description ${DESCRIPTION} -world 1;0;0;0;0;1;0;0;0;0;1;0;0;0;0;1"
env \
  MAYA_LOCATION="${MAYA_LOCATION}" \
  XGEN_LOCATION="${XGEN_ROOT}/" \
  LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${BRIDGE}" --xgen-args "${RENDER_ARGS}" --nxc "${OUTPUT}"
"${ROOT}/build/release/nanoxgen_cache_benchmark" --repeats 11 "${OUTPUT}"

ERROR_DIR="${ROOT}/build/xgen-classic-real"
mkdir -p "${ERROR_DIR}"
MISSING_ARGS="-debug 0 -warning 1 -stats 0 -frame ${FRAME} -shutter 0.0 \
-file ${XGEN_FILE} -palette ${PALETTE} -geom ${GEOMETRY} \
-patch __nanoxgen_missing_patch__ -description ${DESCRIPTION} \
-world 1;0;0;0;0;1;0;0;0;0;1;0;0;0;0;1"
if env \
    MAYA_LOCATION="${MAYA_LOCATION}" \
    XGEN_LOCATION="${XGEN_ROOT}/" \
    LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${BRIDGE}" --xgen-args "${MISSING_ARGS}" \
      >"${ERROR_DIR}/missing-patch.log" 2>&1; then
  echo "Classic bridge unexpectedly accepted a missing patch" >&2
  exit 1
fi
grep -Eq 'PatchRenderer::init returned null|produced zero curves' \
  "${ERROR_DIR}/missing-patch.log"
echo "Classic typed success and missing-patch paths passed"
