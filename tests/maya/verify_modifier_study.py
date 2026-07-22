#!/usr/bin/env python3
"""Check model-independent invariants in the official XGen noise study."""

import argparse
import json
from pathlib import Path


def load(directory: Path, name: str) -> dict:
    with (directory / f"{name}.json").open(encoding="utf-8") as stream:
        return json.load(stream)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()

    names = (
        "noise-zero",
        "noise-reference-a",
        "noise-reference-b",
        "noise-mag-006",
        "noise-mag-024",
        "noise-freq-1125",
        "noise-freq-4500",
        "noise-correlation-000",
        "noise-correlation-100",
        "noise-preserve-000",
        "noise-preserve-100",
        "order-noise-cut",
        "order-cut-noise",
        "repeat-delta",
    )
    studies = {name: load(args.directory, name) for name in names}
    for name, study in studies.items():
        require(study["metadata_mismatches"] == 0, f"{name}: curve identity mismatch")
        require(study["curves"] > 0 and study["cvs_per_curve"] == 17,
                f"{name}: unexpected topology")

    zero = studies["noise-zero"]
    require(zero["relative_displacement"]["max"] <= 5.0e-7,
            "zero-magnitude noise changed positions")
    require(zero["width_absolute_delta"]["max"] <= 1.0e-8,
            "noise changed width at zero magnitude")

    repeat_a = studies["noise-reference-a"]
    repeat_b = studies["noise-reference-b"]
    repeat_delta = studies["repeat-delta"]
    require(repeat_a["displacement_hash"] == repeat_b["displacement_hash"],
            "identical noise fixtures are not deterministic")
    require(repeat_delta["relative_displacement"]["max"] <= 5.0e-7,
            "identical noise fixtures differ numerically")

    for name, study in studies.items():
        if name == "repeat-delta":
            continue
        require(study["root_displacement"]["max"] <= 1.0e-5,
                f"{name}: modifier moved curve roots")

    rms_006 = studies["noise-mag-006"]["relative_displacement"]["rms"]
    rms_012 = repeat_a["relative_displacement"]["rms"]
    rms_024 = studies["noise-mag-024"]["relative_displacement"]["rms"]
    require(0.0 < rms_006 < rms_012 < rms_024,
            "noise magnitude did not monotonically scale displacement")

    require(studies["noise-freq-1125"]["displacement_hash"] !=
            studies["noise-freq-4500"]["displacement_hash"],
            "noise frequency did not change the displacement field")
    require(studies["noise-correlation-000"]["displacement_hash"] !=
            studies["noise-correlation-100"]["displacement_hash"],
            "noise correlation did not change the displacement field")

    preserve_0 = studies["noise-preserve-000"]["arc_length_relative_error"]["mean"]
    preserve_1 = studies["noise-preserve-100"]["arc_length_relative_error"]["mean"]
    require(preserve_1 <= preserve_0 + 1.0e-6,
            "preserveLength=100 did not improve arc-length preservation")
    require(studies["order-noise-cut"]["displacement_hash"] !=
            studies["order-cut-noise"]["displacement_hash"],
            "noise and cut unexpectedly commuted")

    print("official XGen modifier study invariants passed")


if __name__ == "__main__":
    main()
