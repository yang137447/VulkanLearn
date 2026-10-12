"""Validate a lookdev beauty capture and optionally its indirect-specular view.

Reads the ordinary engine screenshot, not a new runtime test command. Values
are display-linear after inverse sRGB, NOT unexposed scene radiance.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from statistics import mean

from bmp_reader import BmpImage


def project(image: BmpImage, x: float, y: float, z: float = 0) -> tuple[int, int]:
    focal = image.width / (2 * math.tan(math.radians(44 / 2)))
    return round(image.width / 2 + focal * x / (630 - z)), round(
        image.height / 2 - focal * (y + 20) / (630 - z))


def sample(image: BmpImage, position: tuple[float, ...]) -> list[float]:
    x, y = project(image, *position)
    pixels = [image.linear_rgb(x + dx, y + dy) for dx in range(-1, 2) for dy in range(-1, 2)]
    return [mean(pixel[channel] for pixel in pixels) for channel in range(3)]


def luminance(rgb: list[float]) -> float:
    return sum(a * b for a, b in zip(rgb, (0.2126, 0.7152, 0.0722)))


def verify(beauty: Path, specular: Path | None, direct: Path | None) -> dict:
    image = BmpImage.load(beauty)
    meters = [sample(image, (x, 32, 1)) for x in (-7, 0, 7)]
    meter_luma = [luminance(value) for value in meters]
    assert all(abs(value - 0.18) <= 0.006 for value in meter_luma), (
        f"18% meter out of calibration: {meter_luma}")
    assert max(meter_luma) - min(meter_luma) <= 0.002, (
        f"Specular=0 still depends on roughness: {meter_luma}")
    assert all(max(value) - min(value) <= 0.002 for value in meters), "Neutral meter has a color cast"
    gray_steps = [luminance(sample(image, (-82.5 + 33 * column, -89))) for column in range(6)]
    assert all(a > b for a, b in zip(gray_steps, gray_steps[1:])), f"Gray steps not monotonic: {gray_steps}"
    ramp = [luminance(sample(image, (-92 + 184 * index / 32, -130))) for index in range(33)]
    assert all(a >= b - 0.001 for a, b in zip(ramp, ramp[1:])), "Reflectance ramp is not monotonic"
    assert ramp[0] - ramp[-1] > 0.5, "Reflectance ramp has insufficient range"
    background = image.linear_rgb(20, image.height // 2)
    assert max(abs(value - 0.04) for value in background) < 0.003, f"Background drift: {background}"
    result = {"beauty": str(beauty), "resolution": [image.width, image.height],
              "measurementSpace": "display-linear after tone mapping, inverse sRGB of 8-bit BMP",
              "grayMeters": meter_luma, "graySteps": gray_steps,
              "rampEndpoints": [ramp[0], ramp[-1]], "background": background}
    if specular:
        spec = BmpImage.load(specular)
        assert (spec.width, spec.height) == (image.width, image.height), "Capture dimensions differ"
        diffuse_positions = [(x, 32, 1) for x in (-7, 0, 7)]
        diffuse_positions += [(49, 68, 45), (0, -130)]
        diffuse_positions += [(-82.5 + 33 * (index % 6), 7 - 32 * (index // 6)) for index in range(24)]
        residual = max(max(sample(spec, position)) for position in diffuse_positions)
        mirror = luminance(sample(spec, (-49, 68, 45)))
        assert residual <= 0.001, f"Diffuse chart/gray sphere has residual specular: {residual}"
        assert mirror > 0.005, "Mirror metal incorrectly lost its specular lobe"
        result.update({"specular": str(specular), "diffuseSpecularResidual": residual,
                       "mirrorSpecular": mirror})
    if direct:
        direct_image = BmpImage.load(direct)
        direct_meters = [luminance(sample(direct_image, (x, 32, 1))) for x in (-7, 0, 7)]
        assert all(abs(value - 0.18) <= 0.006 for value in direct_meters), (
            f"Direct Lambert calibration differs from the analytic prediction: {direct_meters}")
        assert max(direct_meters) - min(direct_meters) <= 0.002, (
            f"Direct Specular=0 still depends on roughness: {direct_meters}")
        result.update({"direct": str(direct), "directGrayMeters": direct_meters})
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("beauty", type=Path)
    parser.add_argument("--specular", type=Path)
    parser.add_argument("--direct", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.beauty, args.specular, args.direct)
    report = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    print(report)
    print("PASS: lookdev gray calibration, roughness independence, neutral background and gray ramp")
