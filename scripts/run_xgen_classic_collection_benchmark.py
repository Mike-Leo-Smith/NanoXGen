#!/usr/bin/env python3
"""Benchmark a complete Classic collection without writing result artifacts."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def run_json_timed(
    command: list[str], environment: dict[str, str] | None = None
) -> tuple[list[dict[str, Any]], float]:
    begin = time.perf_counter()
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    wall_ms = (time.perf_counter() - begin) * 1000.0
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    records: list[dict[str, Any]] = []
    for line in completed.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            records.append(json.loads(line))
    if not records:
        raise RuntimeError(f"command produced no JSON: {command[0]}")
    return records, wall_ms


def run_json(command: list[str], environment: dict[str, str] | None = None) -> list[dict[str, Any]]:
    return run_json_timed(command, environment)[0]


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
        "path": args.cpu_label,
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
        "collection_parse_ms",
        "runtime_plan_lower_ms",
        "alembic_import_ms",
        "root_plan_ms",
        "runtime_inputs_ms",
        "clump_data_ms",
        "guide_rebuild_ms",
        "native_host_pack_ms",
        "jit_compile_allocate_ms",
        "device_buffer_allocate_ms",
        "jit_wait_after_native_ms",
        "jit_compile_active_wall_ms",
        "jit_compile_task_sum_ms",
        "jit_compile_task_max_ms",
        "jit_device_buffer_overlap_ms",
        "upload_ms",
        "host_output_allocate_ms",
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


def summarize_gpu_single_device(args: argparse.Namespace) -> dict[str, Any]:
    command = [
        str(args.luisa_tool),
        str(args.luisa_runtime),
        args.backend,
        str(args.collection),
        str(args.archive),
        str(args.descriptions_root),
        "--warmup",
        str(args.gpu_warmup),
        "--repeats",
        str(args.gpu_repeats),
        "--threads",
        str(args.threads),
        "--no-cpu-validation",
    ]
    if args.motion_samples:
        command.extend(["--frame", str(args.frame), "--fps", str(args.fps)])
        if args.motion_step:
            command.append("--motion-step")
        for lookup, placement in args.motion_samples:
            command.extend([
                "--motion-sample", str(lookup), str(placement)])
    environment = os.environ.copy()
    environment.setdefault("LUISA_LOG_LEVEL", "error")
    if not args.no_outer_warmup:
        run_json(command, environment)
    rounds: list[dict[str, Any]] = []
    for _ in range(args.rounds):
        records = run_json(command, environment)
        summary = next(
            record for record in records if record.get("collection_summary"))
        rounds.append(summary)
    stable_fields = (
        ("description_count", "motion_samples", "sample_strands",
         "sample_points", "unique_deformations", "moving_points",
         "max_motion_position_delta", "checksum", "jit_kernel_count")
        if args.motion_samples else
        ("description_count", "strands", "points", "checksum",
         "jit_kernel_count"))
    validate_stable(rounds, stable_fields, "single-device Luisa collection")
    samples = [float(record["cold_end_to_end_ms"]) for record in rounds]
    component_fields = (
        "device_create_ms",
        "collection_parse_ms",
        "native_prepare_ms",
        "jit_compile_wall_ms",
        "jit_task_sum_ms",
        "jit_task_max_ms",
        "buffer_allocate_ms",
        "upload_ms",
        "first_dispatch_download_pack_ms",
    )
    return {
        "path": f"luisa-{args.backend}-single-device",
        "rounds": args.rounds,
        "outer_warmup": not args.no_outer_warmup,
        "description_process_isolation": False,
        "single_device": True,
        "external_device_api": True,
        "parallel_host_across_descriptions":
            rounds[0]["parallel_host_across_descriptions"],
        "parallel_jit_across_descriptions": True,
        "shader_cache": False,
        "includes_file_io": True,
        "includes_autodesk_serialization": False,
        "descriptions": rounds[0]["description_count"],
        "motion_samples":
            rounds[0].get("motion_samples", 1),
        "unique_deformations":
            rounds[0].get("unique_deformations",
                          rounds[0]["description_count"]),
        "strands":
            rounds[0].get("sample_strands", rounds[0].get("strands")),
        "points":
            rounds[0].get("sample_points", rounds[0].get("points")),
        "moving_points": rounds[0].get("moving_points", 0),
        "max_motion_position_delta":
            rounds[0].get("max_motion_position_delta", 0.0),
        "checksum": rounds[0]["checksum"],
        "context_workers": rounds[0]["context_workers"],
        "jit_kernel_count": rounds[0]["jit_kernel_count"],
        "cold_samples_ms": samples,
        "cold_median_ms": statistics.median(samples),
        "cold_p90_ms": percentile(samples, 0.9),
        "component_median_ms": {
            field: statistics.median(
                float(record[field]) for record in rounds)
            for field in component_fields
        },
    }


def maya_command(args: argparse.Namespace, description: str) -> list[str]:
    patch = args.patch_map[description]
    xgen_args = (
        f"-debug 0 -warning 1 -stats 0 -frame {args.frame} "
        f"-fps {args.fps} -shutter 0.0 "
        f"-file {args.collection} -palette {args.palette} "
        f"-geom {args.archive} -patch {patch} -description {description} "
        "-world 1;0;0;0;0;1;0;0;0;0;1;0;0;0;0;1"
    )
    command = [
        str(args.maya_tool),
        "--xgen-args",
        xgen_args,
        "--description",
        description,
    ]
    if args.motion_samples:
        interpolation = "none" if args.motion_step else "linear"
        lookups = " ".join(str(value[0]) for value in args.motion_samples)
        placements = " ".join(str(value[1]) for value in args.motion_samples)
        command[2] += (
            f" -interpolation {interpolation}"
            f" -motionSamplesLookup {lookups}"
            f" -motionSamplesPlacement {placements}")
        for _, placement in args.motion_samples:
            command.extend(["--shutter-sample", str(placement)])
        command.extend(["--shutter-offset", str(args.shutter_offset)])
    return command


def summarize_maya(args: argparse.Namespace) -> dict[str, Any]:
    if not args.no_outer_warmup:
        for description in args.descriptions:
            print(f"maya warmup: {description}", file=sys.stderr, flush=True)
            run_json(maya_command(args, description))

    rounds: list[list[dict[str, Any]]] = []
    wall_rounds: list[list[float]] = []
    for round_index in range(args.rounds):
        current: list[dict[str, Any]] = []
        current_wall: list[float] = []
        for description in args.descriptions:
            print(
                f"maya round {round_index + 1}/{args.rounds}: {description}",
                file=sys.stderr,
                flush=True,
            )
            records, wall_ms = run_json_timed(
                maya_command(args, description))
            record = records[-1]
            if not record["typed_primitive_cache"] or record["intermediate_xgen_blob"]:
                raise RuntimeError("Maya benchmark did not use the typed direct bridge")
            current.append(record)
            current_wall.append(wall_ms)
        rounds.append(current)
        wall_rounds.append(current_wall)

    by_description: dict[str, list[dict[str, Any]]] = {
        description: [] for description in args.descriptions
    }
    by_description_wall: dict[str, list[float]] = {
        description: [] for description in args.descriptions
    }
    for current, current_wall in zip(rounds, wall_rounds):
        for description, record, wall_ms in zip(
            args.descriptions, current, current_wall
        ):
            by_description[description].append(record)
            by_description_wall[description].append(wall_ms)

    descriptions: dict[str, Any] = {}
    for name in args.descriptions:
        records = by_description[name]
        # Autodesk may schedule PatchRenderer batches in a different source
        # order on every evaluation. Topology must remain stable, but a raw
        # source-order checksum is deliberately diagnostic rather than an
        # invariant; canonical identity validation is a separate oracle.
        validate_stable(
            records,
            ("curves", "points", "motion_samples", "moving_points",
             "max_motion_position_delta"),
            name)
        evaluation = [float(record["evaluation_ms"]) for record in records]
        wall = by_description_wall[name]
        source_checksums = sorted({record["checksum"] for record in records})
        descriptions[name] = {
            "strands": records[0]["curves"],
            "points": records[0]["points"],
            "motion_samples": records[0]["motion_samples"],
            "moving_points": records[0]["moving_points"],
            "max_motion_position_delta":
                records[0]["max_motion_position_delta"],
            "source_checksum_unique_count": len(source_checksums),
            "source_checksums": source_checksums,
            "evaluation_median_ms": statistics.median(evaluation),
            "evaluation_p90_ms": percentile(evaluation, 0.9),
            "process_wall_median_ms": statistics.median(wall),
            "process_wall_p90_ms": percentile(wall, 0.9),
        }

    evaluation_rounds = [
        sum(float(record["evaluation_ms"]) for record in current)
        for current in rounds
    ]
    process_wall_rounds = [sum(current) for current in wall_rounds]
    return {
        "path": "maya-classic-typed",
        "rounds": args.rounds,
        "outer_warmup": not args.no_outer_warmup,
        "description_process_isolation": True,
        "typed_primitive_cache": True,
        "intermediate_xgen_blob": False,
        "includes_file_io": True,
        "includes_cache_write": False,
        "includes_autodesk_serialization": False,
        "descriptions": len(args.descriptions),
        "motion_samples":
            len(args.motion_samples) if args.motion_samples else 1,
        "strands": sum(value["strands"] for value in descriptions.values()),
        "points": sum(value["points"] for value in descriptions.values()),
        "sample_strands": sum(
            value["strands"] * value["motion_samples"]
            for value in descriptions.values()),
        "sample_points": sum(
            value["points"] * value["motion_samples"]
            for value in descriptions.values()),
        "moving_points": sum(
            value["moving_points"] for value in descriptions.values()),
        "max_motion_position_delta": max(
            value["max_motion_position_delta"]
            for value in descriptions.values()),
        "evaluation_samples_ms": evaluation_rounds,
        "evaluation_median_ms": statistics.median(evaluation_rounds),
        "evaluation_p90_ms": percentile(evaluation_rounds, 0.9),
        "process_wall_samples_ms": process_wall_rounds,
        "process_wall_median_ms": statistics.median(process_wall_rounds),
        "process_wall_p90_ms": percentile(process_wall_rounds, 0.9),
        "per_description": descriptions,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu-tool", type=Path)
    parser.add_argument(
        "--cpu-label",
        default="cpu",
        help="path label recorded for a CPU benchmark (for example cpu-portable-release)",
    )
    parser.add_argument("--luisa-tool", type=Path)
    parser.add_argument("--maya-tool", type=Path)
    parser.add_argument("--luisa-runtime", type=Path)
    parser.add_argument("--backend", default="hip")
    parser.add_argument("--frame", type=float, default=1.0)
    parser.add_argument("--fps", type=float, default=24.0)
    parser.add_argument(
        "--motion-sample", action="append", nargs=2, type=float,
        metavar=("LOOKUP", "PLACEMENT"), default=[])
    parser.add_argument("--motion-step", action="store_true")
    parser.add_argument("--shutter-offset", type=float, default=0.0)
    parser.add_argument("--palette")
    parser.add_argument("--patch-map", action="append", default=[])
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--descriptions-root", type=Path, required=True)
    parser.add_argument("--descriptions", nargs="+", default=[])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--gpu-warmup", type=int, default=3)
    parser.add_argument("--gpu-repeats", type=int, default=11)
    parser.add_argument(
        "--threads",
        type=int,
        default=0,
        help="NanoXGenContext pool size; zero uses process CPU affinity",
    )
    parser.add_argument("--single-device-collection", action="store_true")
    parser.add_argument("--no-outer-warmup", action="store_true")
    parser.add_argument("--result-json", type=Path)
    args = parser.parse_args()
    if args.rounds < 1 or args.gpu_warmup < 0 or args.gpu_repeats < 1:
        parser.error("rounds/repeats must be positive and warmup non-negative")
    selected_tools = sum(bool(value) for value in (
        args.cpu_tool, args.luisa_tool, args.maya_tool
    ))
    if selected_tools != 1:
        parser.error(
            "select exactly one of --cpu-tool, --luisa-tool, or --maya-tool")
    if args.luisa_tool and not args.luisa_runtime:
        parser.error("--luisa-runtime is required with --luisa-tool")
    if args.threads < 0:
        parser.error("--threads must be non-negative")
    args.motion_samples = args.motion_sample
    if not (args.fps > 0.0):
        parser.error("--fps must be positive")
    if args.motion_samples:
        if len(args.motion_samples) > 20:
            parser.error("at most 20 --motion-sample values are supported")
        placements = [sample[1] for sample in args.motion_samples]
        if any(
            placements[index] <= placements[index - 1]
            for index in range(1, len(placements))
        ):
            parser.error("motion placements must be strictly increasing")
        if args.luisa_tool and not args.single_device_collection:
            parser.error(
                "Luisa motion requires --single-device-collection")
    if ((args.maya_tool or
         (args.luisa_tool and not args.single_device_collection)) and
            not args.descriptions):
        parser.error("--descriptions is required for Luisa and Maya")
    if args.single_device_collection and not args.luisa_tool:
        parser.error(
            "--single-device-collection requires --luisa-tool")
    if len(set(args.descriptions)) != len(args.descriptions):
        parser.error("--descriptions must not contain duplicates")
    parsed_patch_map: dict[str, str] = {}
    for binding in args.patch_map:
        if "=" not in binding:
            parser.error("--patch-map values must be DESCRIPTION=PATCH")
        description, patch = binding.split("=", 1)
        if not description or not patch or description in parsed_patch_map:
            parser.error("--patch-map contains an invalid or duplicate binding")
        parsed_patch_map[description] = patch
    args.patch_map = parsed_patch_map
    if args.maya_tool:
        if not args.palette:
            parser.error("--palette is required with --maya-tool")
        missing = set(args.descriptions) - set(args.patch_map)
        if missing:
            parser.error(
                "--patch-map is missing descriptions: " + ", ".join(sorted(missing)))
    return args


def main() -> int:
    args = parse_args()
    if args.cpu_tool:
        result = summarize_cpu(args)
    elif args.luisa_tool:
        result = (
            summarize_gpu_single_device(args)
            if args.single_device_collection
            else summarize_gpu(args)
        )
    else:
        result = summarize_maya(args)
    serialized = json.dumps(result, separators=(",", ":"), sort_keys=True)
    if args.result_json:
        args.result_json.write_text(serialized + "\n", encoding="utf-8")
    print(serialized)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
