#!/usr/bin/env python3
"""Benchmark real XGen IGS generation and serialization in one Maya process."""

import argparse
import json
from pathlib import Path
import statistics
import time

import maya.standalone


def summary(samples: list[float]) -> dict[str, float]:
    ordered = sorted(samples)
    p90_index = min(len(ordered) - 1, max(0, int(len(ordered) * 0.9 + 0.999) - 1))
    return {
        "min": ordered[0],
        "median": statistics.median(ordered),
        "p90": ordered[p90_index],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("counts", type=int, nargs="*", default=[10000, 100000])
    args = parser.parse_args()
    if args.repeats <= 0 or any(count <= 0 for count in args.counts):
        raise ValueError("counts and repeats must be positive")
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds
        import maya.api.OpenMaya as om
        import maya.mel as mel

        cmds.loadPlugin("xgenToolkit", quiet=True)
        cases = []
        for requested_count in args.counts:
            cmds.file(new=True, force=True)
            patch = cmds.polyPlane(
                name="BenchmarkPatch",
                width=10.0,
                height=10.0,
                subdivisionsX=10,
                subdivisionsY=10,
            )[0]
            cmds.select(patch, replace=True)
            create_begin = time.perf_counter_ns()
            cmds.xgmCreateSplineDescription(
                createDefaultHair=True,
                name=f"Benchmark{requested_count}",
                density=requested_count / 100.0,
                length=1.0,
                widthScale=0.02,
                cvCount=12,
                generatorSeed=23,
            )
            create_ms = (time.perf_counter_ns() - create_begin) / 1.0e6
            description = (cmds.ls(type="xgmSplineDescription", long=True) or [])[0]
            base = (cmds.ls(type="xgmSplineBase", long=True) or [])[0]
            plug = f"{description}.outRenderData"
            selection = om.MSelectionList()
            selection.add(description)
            output_plug = om.MFnDependencyNode(selection.getDagPath(0).node()).findPlug(
                "outRenderData", False
            )
            blob = args.output.parent / f"xgen-benchmark-{requested_count}.xgen"
            escaped_blob = blob.as_posix().replace('"', r'\"')
            escaped_plug = plug.replace('"', r'\"')

            def export() -> None:
                mel.eval(
                    f'xgmExportSplineDataInternal -output "{escaped_blob}" '
                    f'"{escaped_plug}";'
                )

            export()
            actual_count = cmds.xgmSplineQuery(description, splineCount=True)
            plug_access_ms = []
            cached_export_ms = []
            invalidated_export_ms = []
            for repeat in range(args.repeats):
                cmds.setAttr(f"{base}.generatorSeed", 1000 + repeat * 2)
                begin = time.perf_counter_ns()
                data_object = output_plug.asMObject()
                if data_object.isNull():
                    raise RuntimeError("outRenderData returned a null Maya object")
                plug_access_ms.append((time.perf_counter_ns() - begin) / 1.0e6)

                begin = time.perf_counter_ns()
                export()
                cached_export_ms.append((time.perf_counter_ns() - begin) / 1.0e6)

                cmds.setAttr(f"{base}.generatorSeed", 1001 + repeat * 2)
                begin = time.perf_counter_ns()
                export()
                invalidated_export_ms.append((time.perf_counter_ns() - begin) / 1.0e6)

            cases.append(
                {
                    "requested_strands": requested_count,
                    "strands": actual_count,
                    "cvs": actual_count * 12,
                    "create_ms": create_ms,
                    # outRenderData is a lazy MPxData object. Access time is
                    # recorded to show that it is not the generation cost.
                    "lazy_plug_access_ms": summary(plug_access_ms),
                    "cached_export_ms": summary(cached_export_ms),
                    "invalidated_export_ms": summary(invalidated_export_ms),
                    "invalidated_mcvs_per_s":
                        actual_count * 12 / summary(invalidated_export_ms)["median"] / 1000.0,
                    "blob_bytes": blob.stat().st_size,
                }
            )

        result = {
            "maya_version": cmds.about(version=True),
            "evaluation_mode": cmds.evaluationManager(query=True, mode=True),
            "repeats": args.repeats,
            "cases": cases,
        }
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(json.dumps(result, indent=2, sort_keys=True))
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
