#!/usr/bin/env python3
"""Exercise the in-memory Interactive Groom -> NanoXGen cache command."""

import argparse
import json
from pathlib import Path
import subprocess

import maya.standalone


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--core-cache", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--strands", type=int, default=10000)
    args = parser.parse_args()
    if args.strands <= 0:
        raise ValueError("strands must be positive")
    args.plugin = args.plugin.resolve()
    args.core_cache = args.core_cache.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    source_cache = args.output_dir / "maya-source.nxc"
    canonical_cache = args.output_dir / "maya-canonical.nxc"
    blob = args.output_dir / "oracle.xgen"
    source_oracle = args.output_dir / "disk-source.nxc"
    canonical_oracle = args.output_dir / "disk-canonical.nxc"

    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds
        import maya.mel as mel

        cmds.loadPlugin("xgenToolkit", quiet=True)
        cmds.loadPlugin(str(args.plugin), quiet=True)
        cmds.file(new=True, force=True)
        patch = cmds.polyPlane(
            name="NanoXGenBridgePatch",
            width=10.0,
            height=10.0,
            subdivisionsX=10,
            subdivisionsY=10,
        )[0]
        cmds.select(patch, replace=True)
        cmds.xgmCreateSplineDescription(
            createDefaultHair=True,
            name="NanoXGenBridge",
            density=args.strands / 100.0,
            length=1.0,
            widthScale=0.02,
            cvCount=12,
            generatorSeed=23,
        )
        descriptions = cmds.ls(type="xgmSplineDescription", long=True) or []
        if len(descriptions) != 1:
            raise RuntimeError(f"expected one spline description, found {descriptions}")
        description = descriptions[0]
        actual_strands = cmds.xgmSplineQuery(description, splineCount=True)
        maya_version = cmds.about(version=True)

        source_result = json.loads(
            cmds.nanoxgenXGenCache(description, str(source_cache), "source-order")
        )
        canonical_result = json.loads(
            cmds.nanoxgenXGenCache(description, str(canonical_cache), "canonical")
        )
        escaped_blob = blob.as_posix().replace('"', r'\"')
        escaped_plug = f"{description}.outRenderData".replace('"', r'\"')
        mel.eval(
            f'xgmExportSplineDataInternal -output "{escaped_blob}" "{escaped_plug}";'
        )
    finally:
        maya.standalone.uninitialize()

    subprocess.run(
        [
            str(args.core_cache),
            "--renderer-minimal",
            "--source-order",
            str(blob),
            str(source_oracle),
        ],
        check=True,
    )
    subprocess.run(
        [
            str(args.core_cache),
            "--renderer-minimal",
            str(blob),
            str(canonical_oracle),
        ],
        check=True,
    )
    if source_cache.read_bytes() != source_oracle.read_bytes():
        raise RuntimeError("in-memory source-order cache differs from disk oracle")
    if canonical_cache.read_bytes() != canonical_oracle.read_bytes():
        raise RuntimeError("in-memory canonical cache differs from disk oracle")
    if source_cache.read_bytes() == canonical_cache.read_bytes():
        raise RuntimeError("fixture did not exercise source/canonical order difference")
    if source_result["curves"] != actual_strands:
        raise RuntimeError("bridge curve count differs from Maya spline query")
    for result in (source_result, canonical_result):
        if result["temporary_xgen_blob"] or result["xgfnspline_reload"]:
            raise RuntimeError("bridge reported a forbidden temporary or Autodesk reload")

    report = {
        "maya_version": maya_version,
        "requested_strands": args.strands,
        "strands": actual_strands,
        "source_order": source_result,
        "canonical": canonical_result,
        "source_canonical_differ": True,
        "disk_oracle_bit_exact": True,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
