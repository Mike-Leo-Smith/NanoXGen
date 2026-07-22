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
BUILD_DIR="${NANOXGEN_XGEN_MODIFIER_BUILD_DIR:-${ROOT}/build/xgen-modifier-study}"
MAYAPY="${MAYA_LOCATION}/bin/mayapy"
PROBE="${BUILD_DIR}/nanoxgen_xgen_modifier_probe"

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
  "${MAYAPY}" "${ROOT}/tests/maya/inspect_igs_modifier_schema.py" \
    --output "${BUILD_DIR}/modifier-schema.json"

"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -I"${ROOT}/include" -I"${XGEN_ROOT}/include" \
  "${ROOT}/tools/xgen_modifier_probe.cpp" \
  -L"${XGEN_ROOT}/lib" -L"${MAYA_LOCATION}/lib" \
  -Wl,--allow-shlib-undefined -lAdskXGen -lclew -o "${PROBE}"

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
      --output "${BUILD_DIR}/${name}.xgen" "$@" \
      > "${BUILD_DIR}/${name}-fixture.json"
}

run_probe() {
  local base="$1"
  local target="$2"
  local name="$3"
  LD_LIBRARY_PATH="${RUNTIME_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${PROBE}" "${BUILD_DIR}/${base}.xgen" "${BUILD_DIR}/${target}.xgen" \
    > "${BUILD_DIR}/${name}.json"
}

common=(
  --mesh wave --mesh-width 4 --mesh-height 3 --subdiv-x 8 --subdiv-y 6
  --density 20 --length 1.3 --width 0.025 --cv-count 17 --seed 23
)
noise_reference=(
  --noise-magnitude 0.12 --noise-frequency 2.25
  --noise-correlation 0.35 --noise-preserve-length 0.4
)

run_fixture base "${common[@]}"
run_fixture noise-zero "${common[@]}" \
  --noise-magnitude 0.0 --noise-frequency 2.25 \
  --noise-correlation 0.35 --noise-preserve-length 0.4
run_fixture noise-reference-a "${common[@]}" "${noise_reference[@]}"
run_fixture noise-reference-b "${common[@]}" "${noise_reference[@]}"
run_fixture noise-mag-006 "${common[@]}" "${noise_reference[@]}" --noise-magnitude 0.06
run_fixture noise-mag-024 "${common[@]}" "${noise_reference[@]}" --noise-magnitude 0.24
run_fixture noise-freq-1125 "${common[@]}" "${noise_reference[@]}" --noise-frequency 1.125
run_fixture noise-freq-4500 "${common[@]}" "${noise_reference[@]}" --noise-frequency 4.5
run_fixture noise-correlation-000 "${common[@]}" "${noise_reference[@]}" --noise-correlation 0.0
run_fixture noise-correlation-100 "${common[@]}" "${noise_reference[@]}" --noise-correlation 1.0
run_fixture noise-preserve-000 "${common[@]}" "${noise_reference[@]}" --noise-preserve-length 0.0
run_fixture noise-preserve-100 "${common[@]}" "${noise_reference[@]}" --noise-preserve-length 1.0
run_fixture order-noise-cut "${common[@]}" "${noise_reference[@]}" \
  --cut-percent 27 --modifier-order noise-cut
run_fixture order-cut-noise "${common[@]}" "${noise_reference[@]}" \
  --cut-percent 27 --modifier-order cut-noise

for target in \
  noise-zero noise-reference-a noise-reference-b noise-mag-006 noise-mag-024 \
  noise-freq-1125 noise-freq-4500 noise-correlation-000 noise-correlation-100 \
  noise-preserve-000 noise-preserve-100 order-noise-cut order-cut-noise; do
  run_probe base "${target}" "${target}"
done
run_probe noise-reference-a noise-reference-b repeat-delta

python3 "${ROOT}/tests/maya/verify_modifier_study.py" "${BUILD_DIR}"
echo "XGen modifier study passed: ${BUILD_DIR}"
