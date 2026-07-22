#!/usr/bin/env python3
"""Create a deterministic XGen Interactive Groom fixture in Maya standalone."""

import argparse
import json
from pathlib import Path

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

        patch = cmds.polyPlane(
            name="NanoXGenPatch", width=2.0, height=2.0, subdivisionsX=1, subdivisionsY=1
        )[0]
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
        plug = f"{description}.outRenderData"

        args.output.parent.mkdir(parents=True, exist_ok=True)
        escaped_output = args.output.as_posix().replace('"', r'\"')
        escaped_plug = plug.replace('"', r'\"')
        mel.eval(
            f'xgmExportSplineDataInternal -output "{escaped_output}" "{escaped_plug}";'
        )
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
                },
                sort_keys=True,
            )
        )
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
