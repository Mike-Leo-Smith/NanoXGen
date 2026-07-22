#!/usr/bin/env python3
"""Dump public Maya attribute schemas for candidate IGS modifier nodes."""

import argparse
import json
from pathlib import Path

import maya.standalone


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--node-types",
        nargs="+",
        default=(
            "xgmModifierNoise",
            "xgmModifierCut",
            "xgmModifierClump",
            "xgmModifierCoil",
            "xgmModifierCollision",
        ),
    )
    args = parser.parse_args()
    args.output = args.output.resolve()

    maya.standalone.initialize(name="python")
    try:
        import maya.cmds as cmds

        cmds.file(new=True, force=True)
        cmds.loadPlugin("xgenToolkit", quiet=True)
        result = {"maya_version": cmds.about(version=True), "node_types": {}}
        for node_type in args.node_types:
            try:
                node = cmds.createNode(node_type)
            except RuntimeError as error:
                result["node_types"][node_type] = {
                    "available": False,
                    "error": str(error),
                }
                continue

            actual_type = cmds.nodeType(node)
            if actual_type != node_type:
                result["node_types"][node_type] = {
                    "available": False,
                    "actual_type": actual_type,
                    "error": "Maya created an unknown placeholder for this node type",
                }
                cmds.delete(node)
                continue

            attributes = {}
            skipped_instance_paths = []
            for attribute in sorted(cmds.listAttr(node) or []):
                # listAttr includes paths to children of uninstantiated array
                # compounds (for example
                # magnitudeScale.magnitudeScale_FloatValue).  Such a path is
                # schema metadata, not an addressable plug: it needs a logical
                # array index before getAttr/attributeQuery can resolve it.
                # The compound itself is still reported by listAttr, so retain
                # that entry and record the unresolved child path explicitly.
                if "." in attribute:
                    skipped_instance_paths.append(attribute)
                    continue
                plug = f"{node}.{attribute}"
                try:
                    attribute_type = cmds.getAttr(plug, type=True)
                except (RuntimeError, ValueError):
                    continue
                entry = {
                    "type": attribute_type,
                    "writable": bool(cmds.attributeQuery(attribute, node=node, writable=True)),
                    "readable": bool(cmds.attributeQuery(attribute, node=node, readable=True)),
                    "storable": bool(cmds.attributeQuery(attribute, node=node, storable=True)),
                }
                try:
                    default = cmds.attributeQuery(attribute, node=node, listDefault=True)
                    if default is not None:
                        entry["default"] = default
                except RuntimeError:
                    pass
                if cmds.attributeQuery(attribute, node=node, minExists=True):
                    entry["minimum"] = cmds.attributeQuery(
                        attribute, node=node, minimum=True
                    )
                if cmds.attributeQuery(attribute, node=node, maxExists=True):
                    entry["maximum"] = cmds.attributeQuery(
                        attribute, node=node, maximum=True
                    )
                if attribute_type == "enum":
                    entry["enum"] = cmds.attributeQuery(
                        attribute, node=node, listEnum=True
                    )
                attributes[attribute] = entry
            result["node_types"][node_type] = {
                "available": True,
                "actual_type": actual_type,
                "attributes": attributes,
                "skipped_uninstantiated_compound_paths": skipped_instance_paths,
            }
            cmds.delete(node)

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    finally:
        maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
