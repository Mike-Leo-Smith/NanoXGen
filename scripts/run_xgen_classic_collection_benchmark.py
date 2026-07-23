#!/usr/bin/env python3
"""Benchmark a complete Classic collection without writing result artifacts."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def run_json(command: list[str], environment: dict[str, str] | None = None) -> list[dict[str, Any]]:
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    records: list[dict[str, Any]] = []
    for line in completed.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            records.append(json.loads(line))
    if not records:
        raise RuntimeError(f"command produced no JSON: {command[0]}")
    return records


def validate_stable(records: list[dict[str, Any]], fields: tuple[str, ...], label: str) -> None:
    for field in fields:
        values = {record[field] for record in records}
        if len(values) != 1:
            raise RuntimeError(f"{label} changed {field} between rounds: {sorted(values)}")


def summarize_cpu(args: argparse.Namespace) -> dict[str, Any]:
    command = [
        str(args.cpu_tool),
        str(args.collection),
        str(args.archive),
        str(args.descriptions_root),
        "--generate",
    ]
    if not args.no_outer_warmup:
        run_json(command)
    rounds: list[dict[str, Any]] = []
    for _ in range(args.rounds):
        records = run_json(command)
        summary = next(record for record in records if record.get("summary"))
        rounds.append(summary)
    validate_stable(rounds, ("descriptions", "strands", "points", "checksum"), "CPU")
    samples = [float(record["total_ms"]) for record in rounds]
    first = rounds[0]
    return {
        "path": "cpu-native-lto",
        "rounds": args.rounds,
        "outer_warmup": not args.no_outer_warmup,
        "includes_file_io": True,
        "includes_cache_write": False,
        "descriptions": first["descriptions"],
        "strands": first["strands"],
        "points": first["points"],
        "checksum": first["checksum"],
        "cold_samples_ms": samples,
        "cold_median_ms": statistics.median(samples),
        "cold_p90_ms": percentile(samples, 0.9),
    }


def gpu_command(args: argparse.Namespace, description: str) -> list[str]:
    return [
        str(args.luisa_tool),
        str(args.luisa_runtime),
        args.backend,
        str(args.collection),
        str(args.archive),
        str(args.descriptions_root),
        description,
        "--warmup",
        str(args.gpu_warmup),
        "--repeats",
        str(args.gpu_repeats),
        "--no-cpu-validation",
    ]


def summarize_gpu(args: argparse.Namespace) -> dict[str, Any]:
    environment = os.environ.copy()
    environment.setdefault("LUISA_LOG_LEVEL", "error")
    if not args.no_outer_warmup:
        for description in args.descriptions:
            run_json(gpu_command(args, description), environment)

    rounds: list[list[dict[str, Any]]] = []
    for _ in range(args.rounds):
        current: list[dict[str, Any]] = []
        for description in args.descriptions:
            records = run_json(gpu_command(args, description), environment)
            current.append(records[-1])
        rounds.append(current)

    flattened = [record for current in rounds for record in current]
    if any(record["fallback_count"] != 0 for record in flattened):
        raise RuntimeError("Luisa collection benchmark encountered a fallback")
    if any(record["shader_cache"] for record in flattened):
        raise RuntimeError("Luisa collection benchmark unexpectedly enabled shader cache")
    by_description: dict[str, list[dict[str, Any]]] = {
        description: [] for description in args.descriptions
    }
    for current in rounds:
        for record in current:
            by_description[record["description"]].append(record)
    descriptions: dict[str, Any] = {}
    for name, records in by_description.items():
        validate_stable(records, ("output_strands", "output_points", "checksum"), name)
        cold = [float(record["cold_end_to_end_ms"]) for record in records]
        warm_median = [float(record["warm_median_ms"]) for record in records]
        warm_p90 = [float(record["warm_p90_ms"]) for record in records]
        descriptions[name] = {
            "strands": records[0]["output_strands"],
            "points": records[0]["output_points"],
            "checksum": records[0]["checksum"],
            "cold_median_ms": statistics.median(cold),
            "cold_p90_ms": percentile(cold, 0.9),
            "warm_dispatch_median_ms": statistics.median(warm_median),
            "warm_dispatch_p90_ms": percentile(warm_p90, 0.9),
        }

    component_fields = (
        "device_create_ms",
        "native_parse_import_root_rebuild_ms",
        "jit_compile_allocate_ms",
        "upload_ms",
        "first_dispatch_download_pack_ms",
        "cold_end_to_end_ms",
    )
    aggregate: dict[str, list[float]] = {field: [] for field in component_fields}
    for current in rounds:
        for field in component_fields:
            aggregate[field].append(sum(float(record[field]) for record in current))
    cold = aggregate["cold_end_to_end_ms"]
    return {
        "path": f"luisa-{args.backend}",
        "rounds": args.rounds,
        "outer_warmup": not args.no_outer_warmup,
        "description_process_isolation": True,
        "shader_cache": False,
        "includes_file_io": True,
        "includes_autodesk_serialization": False,
        "gpu_dispatch_warmup": args.gpu_warmup,
        "gpu_dispatch_repeats": args.gpu_repeats,
        "descriptions": len(args.descriptions),
        "strands": sum(value["strands"] for value in descriptions.values()),
        "points": sum(value["points"] for value in descriptions.values()),
        "fallback_count": 0,
        "cold_samples_ms": cold,
        "cold_median_ms": statistics.median(cold),
        "cold_p90_ms": percentile(cold, 0.9),
        "component_median_ms": {
            field: statistics.median(samples)
            for field, samples in aggregate.items()
            if field != "cold_end_to_end_ms"
        },
        "per_description": descriptions,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu-tool", type=Path)
    parser.add_argument("--luisa-tool", type=Path)
    parser.add_argument("--luisa-runtime", type=Path)
    parser.add_argument("--backend", default="hip")
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--descriptions-root", type=Path, required=True)
    parser.add_argument("--descriptions", nargs="+", default=[])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--gpu-warmup", type=int, default=3)
    parser.add_argument("--gpu-repeats", type=int, default=11)
    parser.add_argument("--no-outer-warmup", action="store_true")
    args = parser.parse_args()
    if args.rounds < 1 or args.gpu_warmup < 0 or args.gpu_repeats < 1:
        parser.error("rounds/repeats must be positive and warmup non-negative")
    if bool(args.cpu_tool) == bool(args.luisa_tool):
        parser.error("select exactly one of --cpu-tool or --luisa-tool")
    if args.luisa_tool and not args.luisa_runtime:
        parser.error("--luisa-runtime is required with --luisa-tool")
    if args.luisa_tool and not args.descriptions:
        parser.error("--descriptions is required with --luisa-tool")
    if len(set(args.descriptions)) != len(args.descriptions):
        parser.error("--descriptions must not contain duplicates")
    return args


def main() -> int:
    args = parse_args()
    result = summarize_cpu(args) if args.cpu_tool else summarize_gpu(args)
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
