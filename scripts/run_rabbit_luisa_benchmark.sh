#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 LUISA_RUNTIME_DIR RABBIT_COLLECTION.xgen RABBIT_NXG_DIR" >&2
  exit 2
fi

runtime_dir=$1
collection=$2
asset_dir=$3
benchmark=${NANOXGEN_LUISA_BENCHMARK:-./build/luisa-hip-release/nanoxgen_luisa_classic_benchmark}
backend=${NANOXGEN_LUISA_BACKEND:-hip}
warmup=${NANOXGEN_BENCHMARK_WARMUP:-3}
repeats=${NANOXGEN_BENCHMARK_REPEATS:-15}
cpu_repeats=${NANOXGEN_CPU_BENCHMARK_REPEATS:-3}

exec "$benchmark" "$runtime_dir" "$backend" "$collection" \
  --case "mm,$asset_dir/rabbit-mm-subd.nxg,3526,17" \
  --case "erduo,$asset_dir/rabbit-erduo-subd.nxg,339574,17" \
  --case "body,$asset_dir/rabbit-body-subd.nxg,330038,27" \
  --case "eyelash,$asset_dir/rabbit-eyelash-subd.nxg,1514,17" \
  --case "head,$asset_dir/rabbit-head-subd.nxg,1150937,17" \
  --case "nose,$asset_dir/rabbit-nose-subd.nxg,67231,17" \
  --case "sizhi,$asset_dir/rabbit-sizhi-subd.nxg,236693,27" \
  --case "weiba,$asset_dir/rabbit-weiba-subd.nxg,18835,17" \
  --case "head_A,$asset_dir/rabbit-head_A-subd.nxg,307791,17" \
  --warmup "$warmup" --repeats "$repeats" --cpu-repeats "$cpu_repeats"
