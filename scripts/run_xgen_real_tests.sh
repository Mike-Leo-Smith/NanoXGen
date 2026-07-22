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
MAYAPY="${MAYA_LOCATION}/bin/mayapy"
PROBE="${BUILD_DIR}/nanoxgen_xgen_probe"
PARITY="${BUILD_DIR}/nanoxgen_xgen_parity"

"${ROOT}/scripts/check_xgen_sdk.sh" "${XGEN_ROOT}"
if [[ ! -x "${MAYAPY}" ]]; then
  echo "mayapy not found at ${MAYAPY}" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}" "${BUILD_DIR}/maya-home" "${BUILD_DIR}/maya-app"

RUNTIME_LIBS="${XGEN_ROOT}/lib:${MAYA_LOCATION}/lib"
if [[ -n "${COMPAT_LIB_DIR}" ]]; then
  RUNTIME_LIBS="${RUNTIME_LIBS}:${COMPAT_LIB_DIR}"
fi

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I"${XGEN_ROOT}/include" \
  "${ROOT}/tools/xgen_probe.cpp" \
  -L"${XGEN_ROOT}/lib" -L"${MAYA_LOCATION}/lib" \
  -Wl,--allow-shlib-undefined \
  -lAdskXGen -lclew -o "${PROBE}"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread \
  -I"${ROOT}/include" -I"${XGEN_ROOT}/include" \
  "${ROOT}/src/asset.cpp" "${ROOT}/tools/xgen_parity.cpp" \
  -L"${XGEN_ROOT}/lib" -L"${MAYA_LOCATION}/lib" \
  -Wl,--allow-shlib-undefined \
  -lAdskXGen -lclew -o "${PARITY}"

run_fixture() {
  local name="$1"
  shift
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
    "${MAYAPY}" "${ROOT}/tests/maya/create_igs_fixture.py" \
      --output "${BUILD_DIR}/${name}.xgen" "$@"
  LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${PROBE}" "${BUILD_DIR}/${name}.xgen" > "${BUILD_DIR}/${name}.json"
}

run_fixture baseline-a
run_fixture baseline-b
run_fixture variant --density 2 --length 0.5 --width 0.03 --cv-count 7 --seed 19
complex_args=(
  --mesh wave --mesh-width 4 --mesh-height 3 --subdiv-x 8 --subdiv-y 6
  --density 20 --length 1.3 --width 0.025 --cv-count 9 --seed 23
)
run_fixture complex-base "${complex_args[@]}"
run_fixture complex-base-repeat "${complex_args[@]}"
run_fixture complex-cut-taper "${complex_args[@]}" \
  --cut-percent 27 --width-taper 0.8 --width-taper-start 0.25
run_fixture complex-chain "${complex_args[@]}" \
  --noise-magnitude 0.12 --noise-frequency 2.25 --noise-correlation 0.35 \
  --noise-preserve-length 0.4 --cut-percent 27

python3 "${ROOT}/tests/maya/verify_igs_summaries.py" \
  "${BUILD_DIR}/baseline-a.json" \
  "${BUILD_DIR}/baseline-b.json" \
  "${BUILD_DIR}/variant.json" \
  --complex-base "${BUILD_DIR}/complex-base.json" \
  --complex-base-repeat "${BUILD_DIR}/complex-base-repeat.json" \
  --complex-cut-taper "${BUILD_DIR}/complex-cut-taper.json" \
  --complex-chain "${BUILD_DIR}/complex-chain.json"

LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${PARITY}" "${BUILD_DIR}/complex-base.xgen" "${BUILD_DIR}/complex-base.xgen" \
  > "${BUILD_DIR}/parity-base.json"
LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${PARITY}" "${BUILD_DIR}/complex-base.xgen" "${BUILD_DIR}/complex-cut-taper.xgen" \
  --length-scale 0.73 --width-taper 0.8 --width-taper-start 0.25 \
  > "${BUILD_DIR}/parity-cut-taper.json"

python3 -c 'from pathlib import Path; Path(__import__("sys").argv[1]).write_bytes(b"")' \
  "${BUILD_DIR}/malformed.xgen"
if LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${PROBE}" "${BUILD_DIR}/malformed.xgen" > "${BUILD_DIR}/malformed.log" 2>&1; then
  echo "malformed XGen BLOB unexpectedly passed" >&2
  exit 1
fi

echo "all real XGen tests passed"
