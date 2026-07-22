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
REPEATS="${NANOXGEN_BRIDGE_REPEATS:-5}"
STRANDS="${NANOXGEN_BRIDGE_STRANDS:-100000}"
PLUGIN="${BUILD_DIR}/nanoxgen_maya_xgen_profile.so"
mkdir -p "${BUILD_DIR}" "${BUILD_DIR}/maya-home" "${BUILD_DIR}/maya-app"

RUNTIME_LIBS="${XGEN_ROOT}/lib:${MAYA_LOCATION}/lib"
if [[ -n "${COMPAT_LIB_DIR}" ]]; then
  RUNTIME_LIBS="${RUNTIME_LIBS}:${COMPAT_LIB_DIR}"
fi

"${CXX:-c++}" -std=c++17 -O3 -DNDEBUG -fPIC -shared \
  -Wall -Wextra -Wpedantic \
  -I"${MAYA_LOCATION}/include" -I"${XGEN_ROOT}/include" \
  "${ROOT}/tools/maya_xgen_profile.cpp" \
  -L"${MAYA_LOCATION}/lib" -L"${XGEN_ROOT}/lib" \
  -Wl,--allow-shlib-undefined \
  -lOpenMaya -lFoundation -lAdskXGen -lclew -o "${PLUGIN}"

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
  "${MAYA_LOCATION}/bin/mayapy" "${ROOT}/tests/maya/profile_igs_bridge.py" \
    --plugin "${PLUGIN}" \
    --output "${BUILD_DIR}/xgen-bridge-profile.json" \
    --strands "${STRANDS}" --repeats "${REPEATS}"

echo "XGen bridge profile: ${BUILD_DIR}/xgen-bridge-profile.json"
