#!/usr/bin/env bash
set -euo pipefail

XGEN_ROOT="${1:-${XGEN_ROOT:-}}"
if [[ -z "${XGEN_ROOT}" ]]; then
  echo "usage: $0 <Maya>/plug-ins/xgen" >&2
  echo "or set XGEN_ROOT in the environment" >&2
  exit 2
fi

headers=(
  "${XGEN_ROOT}/include/XGen/XgSplineAPI.h"
  "${XGEN_ROOT}/include/xgen/src/xgcore/XgConfig.h"
)
header=''
for candidate in "${headers[@]}"; do
  if [[ -f "${candidate}" ]]; then header="${candidate}"; break; fi
done
library="${XGEN_ROOT}/lib/libAdskXGen.so"
maya_library="$(dirname "$(dirname "${XGEN_ROOT}")")/lib/libclew.so"

if [[ -z "${header}" || ! -f "${library}" || ! -f "${maya_library}" ]]; then
  echo "not a usable XGen SDK root: ${XGEN_ROOT}" >&2
  echo "expected XgSplineAPI/XgConfig headers and ${library}" >&2
  exit 1
fi

echo "XGen SDK root: ${XGEN_ROOT}"
echo "header:        ${header}"
echo "library:       ${library}"
echo "Maya runtime:  ${maya_library}"
echo "configure:     cmake -S . -B build -DXGEN_ROOT=${XGEN_ROOT}"
