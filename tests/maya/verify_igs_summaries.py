#!/usr/bin/env python3
"""Validate canonical summaries produced by nanoxgen_xgen_probe."""

import argparse
import json
import math
from pathlib import Path


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def check_fixture(
    summary: dict, *, curves: int, vertices_per_curve: int, width: float, height: float
) -> None:
    require(summary["valid"] is True, "probe did not validate the XGen data")
    require(summary["motion_samples"] == 1, "unexpected motion sample count")
    require(summary["batches"] == 1, "unexpected spline batch count")
    require(summary["curves"] == curves, "unexpected curve count")
    require(summary["vertices"] == curves * vertices_per_curve, "unexpected vertex count")
    require(
        summary["vertices_per_curve"] == {
            "min": vertices_per_curve,
            "max": vertices_per_curve,
        },
        "unexpected CV count",
    )
    require(math.isclose(summary["width"]["min"], width, abs_tol=1.0e-6), "bad min width")
    require(math.isclose(summary["width"]["max"], width, abs_tol=1.0e-6), "bad max width")
    require(
        abs(summary["position_bounds"]["min"][1]) <= 1.0e-5,
        "roots are not on the source plane",
    )
    require(
        math.isclose(summary["position_bounds"]["max"][1], height, abs_tol=1.0e-5),
        "tips do not match the requested length",
    )
    patch_uvs = summary["patch_uv_bounds"]["min"] + summary["patch_uv_bounds"]["max"]
    require(all(0.0 <= value <= 1.0 for value in patch_uvs), "patch UV is outside [0, 1]")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline_a", type=Path)
    parser.add_argument("baseline_b", type=Path)
    parser.add_argument("variant", type=Path)
    args = parser.parse_args()

    baseline_a = load(args.baseline_a)
    baseline_b = load(args.baseline_b)
    variant = load(args.variant)
    check_fixture(baseline_a, curves=16, vertices_per_curve=5, width=0.02, height=1.0)
    check_fixture(baseline_b, curves=16, vertices_per_curve=5, width=0.02, height=1.0)
    check_fixture(variant, curves=8, vertices_per_curve=7, width=0.03, height=0.5)
    require(
        baseline_a["canonical_hash"] == baseline_b["canonical_hash"],
        "same seed and inputs did not reproduce identical canonical geometry",
    )
    require(
        baseline_a["canonical_hash"] != variant["canonical_hash"],
        "different fixture unexpectedly produced the same canonical geometry",
    )
    print("real XGen fixture summaries passed")


if __name__ == "__main__":
    main()
