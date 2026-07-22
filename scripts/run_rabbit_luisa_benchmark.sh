#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  echo "usage: $0 LUISA_RUNTIME_DIR RABBIT_COLLECTION.xgen PATCHES.abc DESCRIPTIONS_ROOT [REFERENCE.nxc]" >&2
  exit 2
fi

runtime_dir=$1
collection=$2
archive=$3
descriptions_root=$4
reference=${5:-}
benchmark=${NANOXGEN_LUISA_BENCHMARK:-./build/luisa-classic-hip-release/nanoxgen_xgen_classic_luisa_benchmark}
backend=${NANOXGEN_LUISA_BACKEND:-hip}
warmup=${NANOXGEN_BENCHMARK_WARMUP:-3}
repeats=${NANOXGEN_BENCHMARK_REPEATS:-11}

args=("$runtime_dir" "$backend" "$collection" "$archive"
      "$descriptions_root" eyelash --warmup "$warmup" --repeats "$repeats")
if [[ -n "$reference" ]]; then
  args+=(--reference-nxc "$reference")
fi
exec "$benchmark" "${args[@]}"
