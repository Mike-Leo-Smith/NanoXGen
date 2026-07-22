#!/usr/bin/env python3
"""Numerically verify the independently implemented XGen noise model."""

import argparse
import json
import math
import re
from pathlib import Path


ROOT_OFFSET = (0.419276, 0.184247, 0.805721)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_field(directory: Path, name: str) -> list[dict]:
    with (directory / f"{name}-field.json").open(encoding="utf-8") as stream:
        document = json.load(stream)
    require(document["metadata_mismatches"] == 0, f"{name}: identity mismatch")
    return document["field"]


def curve_key(curve: dict) -> tuple:
    return (curve["face_id"], *curve["face_uv"], *curve["patch_uv"])


def parse_gradients(path: Path) -> list[tuple[float, float, float]]:
    pattern = re.compile(
        r"\{\s*([-+0-9.eE]+)f\s*,\s*([-+0-9.eE]+)f\s*,"
        r"\s*([-+0-9.eE]+)f\s*\}")
    gradients = [tuple(map(float, match)) for match in pattern.findall(path.read_text())]
    require(len(gradients) == 256, "expected the 256-entry SeExpr gradient table")
    return gradients


def noise_hash(x: int, y: int, z: int) -> int:
    seed = 0
    for coordinate in (x, y, z):
        seed = (seed * 1664525 + (coordinate & 0xFFFFFFFF) + 1013904223) & 0xFFFFFFFF
    seed ^= seed >> 11
    seed &= 0xFFFFFFFF
    seed ^= (seed << 7) & 0x9D2C5680
    seed &= 0xFFFFFFFF
    seed ^= (seed << 15) & 0xEFC60000
    seed &= 0xFFFFFFFF
    seed ^= seed >> 18
    return (((seed & 0x00FF0000) >> 4) + (seed & 0xFF)) & 0xFF


def gradient_noise(
        gradients: list[tuple[float, float, float]], point: tuple[float, float, float]) -> float:
    cell = tuple(math.floor(value) for value in point)
    weights = tuple(point[axis] - cell[axis] for axis in range(3))
    values = []
    for corner in range(8):
        offset = (corner & 1, (corner >> 1) & 1, (corner >> 2) & 1)
        gradient = gradients[noise_hash(*(cell[axis] + offset[axis] for axis in range(3)))]
        values.append(sum(
            gradient[axis] * (weights[axis] - offset[axis]) for axis in range(3)))
    alphas = tuple(value ** 3 * (value * (value * 6.0 - 15.0) + 10.0)
                   for value in weights)
    for dimension in range(2, -1, -1):
        for value in range(1 << dimension):
            index = value * (1 << (3 - dimension))
            axis = 2 - dimension
            other = index + (1 << axis)
            values[index] = ((1.0 - alphas[axis]) * values[index] +
                             alphas[axis] * values[other])
    return 0.5 * values[0] + 0.5


def distance(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((a[axis] - b[axis]) ** 2 for axis in range(3)))


def arc_length(points: list[list[float]]) -> float:
    return sum(distance(points[index], points[index - 1])
               for index in range(1, len(points)))


def local_noise_field(
        gradients: list[tuple[float, float, float]], curve: dict,
        correlation: float, frequency: float, magnitude: float) -> list[tuple[float, float, float]]:
    base = curve["base"]
    root = base[0]
    correlation_scale = 100.0 * (1.0 - correlation) ** 2
    domain = tuple((root[axis] + ROOT_OFFSET[axis]) * correlation_scale
                   for axis in range(3))
    length = arc_length(base)
    effective_frequency = max(0.5 / length, frequency)
    result = []
    travelled = 0.0
    for index, point in enumerate(base):
        if index:
            travelled += distance(point, base[index - 1])
        sample_distance = travelled * effective_frequency
        amount = magnitude * index / (len(base) - 1)
        result.append(tuple(
            (gradient_noise(gradients, tuple(
                domain[component] + (sample_distance if axis == component else 0.0)
                for component in range(3))) - 0.5) * amount
            for axis in range(3)))
    return result


def fit_flat_frame(
        gradients: list[tuple[float, float, float]], curve: dict) -> tuple[float, float]:
    local = local_noise_field(gradients, curve, 1.0, 2.25, 0.12)
    cosine_numerator = 0.0
    sine_numerator = 0.0
    for vector, base, target in zip(local, curve["base"], curve["target"]):
        world_x = target[0] - base[0]
        world_z = target[2] - base[2]
        cosine_numerator += vector[0] * world_x + vector[1] * world_z
        sine_numerator += vector[0] * world_z - vector[1] * world_x
    norm = math.hypot(cosine_numerator, sine_numerator)
    require(norm > 1.0e-12, "unable to recover flat XGen surface frame")
    return cosine_numerator / norm, sine_numerator / norm


def model_errors(
        gradients: list[tuple[float, float, float]], curves: list[dict],
        frames: dict[tuple, tuple[float, float]], correlation: float,
        frequency: float, magnitude: float) -> tuple[float, float]:
    squared_error = 0.0
    maximum_error = 0.0
    count = 0
    for curve in curves:
        cosine, sine = frames[curve_key(curve)]
        local = local_noise_field(gradients, curve, correlation, frequency, magnitude)
        for vector, base, target in zip(local, curve["base"], curve["target"]):
            prediction = (
                cosine * vector[0] - sine * vector[1],
                vector[2],
                sine * vector[0] + cosine * vector[1],
            )
            for axis in range(3):
                error = (target[axis] - base[axis]) - prediction[axis]
                squared_error += error * error
                maximum_error = max(maximum_error, abs(error))
                count += 1
    return math.sqrt(squared_error / count), maximum_error


def compare_fields(left: list[dict], right: list[dict]) -> float:
    right_by_key = {curve_key(curve): curve for curve in right}
    maximum = 0.0
    for curve in left:
        other = right_by_key[curve_key(curve)]
        for a, b in zip(curve["target"], other["target"]):
            maximum = max(maximum, *(abs(a[axis] - b[axis]) for axis in range(3)))
    return maximum


def preservation_error(noisy: list[dict], preserved: list[dict], amount: float) -> tuple[float, float]:
    noisy_by_key = {curve_key(curve): curve for curve in noisy}
    squared_error = 0.0
    maximum_error = 0.0
    count = 0
    for curve in preserved:
        source = noisy_by_key[curve_key(curve)]
        root = source["base"][0]
        base_length = arc_length(source["base"])
        noisy_length = arc_length(source["target"])
        target_length = amount * base_length + (1.0 - amount) * noisy_length
        scale = target_length / noisy_length
        if abs(noisy_length - target_length) < 1.0e-4:
            scale = 1.0
        for noisy_point, actual in zip(source["target"], curve["target"]):
            prediction = tuple(root[axis] + (noisy_point[axis] - root[axis]) * scale
                               for axis in range(3))
            for axis in range(3):
                error = actual[axis] - prediction[axis]
                squared_error += error * error
                maximum_error = max(maximum_error, abs(error))
                count += 1
    return math.sqrt(squared_error / count), maximum_error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("gradient_header", type=Path)
    args = parser.parse_args()
    gradients = parse_gradients(args.gradient_header)

    reference = load_field(args.directory, "flat-correlation-100")
    frames = {curve_key(curve): fit_flat_frame(gradients, curve) for curve in reference}
    correlation_results = {}
    for label, ui_value in (
            ("000", 0), ("025", 25), ("050", 50), ("075", 75),
            ("090", 90), ("095", 95), ("099", 99), ("100", 100)):
        errors = model_errors(
            gradients, load_field(args.directory, f"flat-correlation-{label}"),
            frames, ui_value / 100.0, 2.25, 0.12)
        correlation_results[label] = errors
        require(errors[0] < 2.0e-7 and errors[1] < 3.0e-6,
                f"correlation={ui_value}: gradient-noise model mismatch {errors}")

    for label, magnitude in (("006", 0.06), ("024", 0.24)):
        errors = model_errors(
            gradients, load_field(args.directory, f"flat-magnitude-{label}"),
            frames, 0.75, 2.25, magnitude)
        require(errors[0] < 2.0e-7 and errors[1] < 3.0e-6,
                f"magnitude={magnitude}: gradient-noise model mismatch {errors}")

    frequency_fields = {
        label: load_field(args.directory, f"flat-frequency-{label}")
        for label in ("0000", "0100", "floor", "0500")
    }
    require(compare_fields(frequency_fields["0000"], frequency_fields["0100"]) < 5.0e-7 and
            compare_fields(frequency_fields["0000"], frequency_fields["floor"]) < 5.0e-7,
            "frequencies below 0.5/length did not clamp to the same field")
    require(compare_fields(frequency_fields["floor"], frequency_fields["0500"]) > 1.0e-5,
            "frequency above the minimum did not change the field")
    for label, frequency in (("0000", 0.0), ("0100", 0.1),
                             ("floor", 0.5 / 1.3), ("0500", 0.5)):
        errors = model_errors(
            gradients, frequency_fields[label], frames, 0.75, frequency, 0.12)
        require(errors[0] < 2.0e-7 and errors[1] < 3.0e-6,
                f"frequency={frequency}: gradient-noise model mismatch {errors}")

    noisy = load_field(args.directory, "noise-preserve-000")
    preserve_100 = preservation_error(
        noisy, load_field(args.directory, "noise-preserve-100"), 1.0)
    preserve_40 = preservation_error(
        noisy, load_field(args.directory, "noise-reference-a"), 0.4)
    require(preserve_100[0] < 5.0e-7 and preserve_100[1] < 3.0e-6,
            f"preserveLength=100 is not uniform root scaling: {preserve_100}")
    require(preserve_40[0] < 5.0e-7 and preserve_40[1] < 3.0e-6,
            f"preserveLength=40 is not uniform root scaling: {preserve_40}")

    worst_rms = max(value[0] for value in correlation_results.values())
    worst_max = max(value[1] for value in correlation_results.values())
    print(f"official XGen noise model matched: correlation RMS <= {worst_rms:.3g}, "
          f"max <= {worst_max:.3g}; preserve100 RMS={preserve_100[0]:.3g}; "
          f"preserve40 RMS={preserve_40[0]:.3g}")


if __name__ == "__main__":
    main()
