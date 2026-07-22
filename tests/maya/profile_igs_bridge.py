#!/usr/bin/env python3
"""Profile the exact Maya MPxData -> XgFnSpline renderer bridge."""

import argparse
import json
from pathlib import Path
import statistics

import maya.standalone


def summary(values: list[float]) -> dict[str, float]:
    ordered = sorted(values)
    p90 = min(len(ordered) - 1, max(0, int(len(ordered) * 0.9 + 0.999) - 1))
    return {
        "min": ordered[0],
        "median": statistics.median(ordered),
        "p90": ordered[p90],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--strands", type=int, default=100000)
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if args.strands <= 0 or args.repeats <= 0:
        raise ValueError("strands and repeats must be positive")
    args.plugin = args.plugin.resolve()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds

        cmds.loadPlugin("xgenToolkit", quiet=True)
        cmds.loadPlugin(str(args.plugin), quiet=True)
        cmds.file(new=True, force=True)
        patch = cmds.polyPlane(
            name="BridgeProfilePatch",
            width=10.0,
            height=10.0,
            subdivisionsX=10,
            subdivisionsY=10,
        )[0]
        cmds.select(patch, replace=True)
        cmds.xgmCreateSplineDescription(
            createDefaultHair=True,
            name="BridgeProfile",
            density=args.strands / 100.0,
            length=1.0,
            widthScale=0.02,
            cvCount=12,
            generatorSeed=23,
        )
        description = (cmds.ls(type="xgmSplineDescription", long=True) or [])[0]
        base = (cmds.ls(type="xgmSplineBase", long=True) or [])[0]
        actual_strands = cmds.xgmSplineQuery(description, splineCount=True)

        modes = ("stringstream-copy", "stringstream-no-copy", "vector")
        rows: dict[str, list[dict]] = {mode: [] for mode in modes}
        reserve = 0
        for repeat in range(args.repeats):
            for mode_index, mode in enumerate(modes):
                cmds.setAttr(
                    f"{base}.generatorSeed",
                    1000 + repeat * len(modes) + mode_index,
                )
                row = json.loads(
                    cmds.nanoxgenProfileXGenData(description, mode, reserve)
                )
                rows[mode].append(row)
                reserve = max(reserve, row["blob_bytes"])

        stages = (
            "evaluate_ms",
            "write_binary_ms",
            "stream_copy_ms",
            "load_ms",
            "execute_ms",
            "materialize_ms",
            "total_ms",
        )
        result = {
            "maya_version": cmds.about(version=True),
            "requested_strands": args.strands,
            "strands": actual_strands,
            "cvs": actual_strands * 12,
            "repeats": args.repeats,
            "modes": {
                mode: {
                    "blob_bytes": rows[mode][0]["blob_bytes"],
                    "batches": rows[mode][0]["batches"],
                    **{
                        stage: summary([row[stage] for row in rows[mode]])
                        for stage in stages
                    },
                }
                for mode in modes
            },
        }
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(json.dumps(result, indent=2, sort_keys=True))
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
