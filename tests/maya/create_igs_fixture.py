#!/usr/bin/env python3
"""Create a deterministic XGen Interactive Groom fixture in Maya standalone."""

import argparse
import json
import math
from pathlib import Path
import time

import maya.standalone


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scene", type=Path)
    parser.add_argument("--density", type=float, default=4.0)
    parser.add_argument("--length", type=float, default=1.0)
    parser.add_argument("--width", type=float, default=0.02)
    parser.add_argument("--cv-count", type=int, default=5)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--mesh", choices=("plane", "wave"), default="plane")
    parser.add_argument("--mesh-width", type=float, default=2.0)
    parser.add_argument("--mesh-height", type=float, default=2.0)
    parser.add_argument("--subdiv-x", type=int, default=1)
    parser.add_argument("--subdiv-y", type=int, default=1)
    parser.add_argument("--width-taper", type=float, default=0.0)
    parser.add_argument("--width-taper-start", type=float, default=0.0)
    parser.add_argument("--cut-percent", type=float)
    parser.add_argument("--noise-magnitude", type=float)
    parser.add_argument("--noise-frequency", type=float, default=1.0)
    parser.add_argument("--noise-correlation", type=float, default=0.0)
    parser.add_argument("--noise-preserve-length", type=float, default=0.0)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.scene:
        args.scene = args.scene.resolve()

    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds
        import maya.mel as mel

        cmds.file(new=True, force=True)
        cmds.loadPlugin("xgenToolkit", quiet=True)

        fixture_start = time.perf_counter_ns()
        patch = cmds.polyPlane(
            name="NanoXGenPatch",
            width=args.mesh_width,
            height=args.mesh_height,
            subdivisionsX=args.subdiv_x,
            subdivisionsY=args.subdiv_y,
        )[0]
        if args.mesh == "wave":
            for vertex in cmds.ls(f"{patch}.vtx[*]", flatten=True):
                x, _, z = cmds.xform(
                    vertex, query=True, worldSpace=True, translation=True
                )
                y = 0.18 * math.sin(1.7 * x) * math.cos(1.3 * z) + 0.04 * x * z
                cmds.xform(vertex, worldSpace=True, translation=(x, y, z))
            cmds.polySoftEdge(patch, angle=180.0, constructionHistory=False)
        cmds.select(patch, replace=True)
        cmds.xgmCreateSplineDescription(
            createDefaultHair=True,
            name="NanoXGenFixture",
            density=args.density,
            length=args.length,
            widthScale=args.width,
            cvCount=args.cv_count,
            generatorSeed=args.seed,
        )

        descriptions = cmds.ls(type="xgmSplineDescription", long=True) or []
        if len(descriptions) != 1:
            raise RuntimeError(f"expected one spline description, found {descriptions}")
        description = descriptions[0]
        cmds.setAttr(f"{description}.widthTaper", args.width_taper)
        cmds.setAttr(f"{description}.widthTaperStart", args.width_taper_start)

        def insert_modifier(node_type: str, attributes: dict[str, object]) -> str:
            source = cmds.connectionInfo(
                f"{description}.inSplineData", sourceFromDestination=True
            )
            if not source:
                raise RuntimeError("description has no upstream spline source")
            node = cmds.createNode(
                node_type, name=node_type.removeprefix("xgmModifier").lower()
            )
            cmds.disconnectAttr(source, f"{description}.inSplineData")
            cmds.connectAttr(source, f"{node}.inSplineData")
            cmds.connectAttr(f"{node}.outSplineData", f"{description}.inSplineData")
            for attribute, value in attributes.items():
                cmds.setAttr(f"{node}.{attribute}", value)
            return node

        modifiers = []
        if args.noise_magnitude is not None:
            modifiers.append(
                insert_modifier(
                    "xgmModifierNoise",
                    {
                        "magnitude": args.noise_magnitude,
                        "frequency": args.noise_frequency,
                        "correlation": args.noise_correlation,
                        "preserveLength": args.noise_preserve_length,
                    },
                )
            )
        if args.cut_percent is not None:
            modifiers.append(
                insert_modifier(
                    "xgmModifierCut",
                    {
                        "cutMode": 1,
                        "percentage": args.cut_percent,
                        "redistributingCV": True,
                    },
                )
            )
        plug = f"{description}.outRenderData"

        args.output.parent.mkdir(parents=True, exist_ok=True)
        escaped_output = args.output.as_posix().replace('"', r'\"')
        escaped_plug = plug.replace('"', r'\"')
        export_start = time.perf_counter_ns()
        mel.eval(
            f'xgmExportSplineDataInternal -output "{escaped_output}" "{escaped_plug}";'
        )
        export_ms = (time.perf_counter_ns() - export_start) / 1.0e6
        if not args.output.is_file() or args.output.stat().st_size == 0:
            raise RuntimeError("XGen export did not produce a non-empty BLOB")

        if args.scene:
            args.scene.parent.mkdir(parents=True, exist_ok=True)
            cmds.file(rename=str(args.scene))
            cmds.file(save=True, type="mayaAscii", force=True)

        print(
            json.dumps(
                {
                    "maya_version": cmds.about(version=True),
                    "description": description,
                    "spline_count": cmds.xgmSplineQuery(description, splineCount=True),
                    "blob": str(args.output),
                    "blob_bytes": args.output.stat().st_size,
                    "mesh": args.mesh,
                    "modifiers": modifiers,
                    "fixture_ms": (time.perf_counter_ns() - fixture_start) / 1.0e6,
                    "export_ms": export_ms,
                },
                sort_keys=True,
            )
        )
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
