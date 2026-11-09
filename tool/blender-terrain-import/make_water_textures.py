"""Generate a small placeholder PBR water texture set for the river layer."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resolution", type=int, default=1024)
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    side = args.resolution
    y, x = np.mgrid[0:side, 0:side]
    u = x / max(side - 1, 1)
    v = y / max(side - 1, 1)
    # Gentle, deterministic ripples; no animation in this first pass.
    ripple = (0.5 + 0.5 * np.sin(2.0 * np.pi * (u * 11.0 + v * 3.0))
              * np.sin(2.0 * np.pi * (v * 7.0 - u * 2.0)))
    albedo = np.zeros((side, side, 3), dtype=np.float64)
    albedo[:, :, 0] = 0.035 + 0.025 * ripple
    albedo[:, :, 1] = 0.090 + 0.040 * ripple
    albedo[:, :, 2] = 0.145 + 0.060 * ripple
    albedo8 = (np.clip(albedo, 0.0, 1.0) * 255.0).astype(np.uint8)
    Image.fromarray(albedo8, mode="RGB").save(
        args.output / "water_albedo.png")

    normal = np.zeros((side, side, 3), dtype=np.uint8)
    normal[:, :, 0] = np.clip(128.0 + 6.0 * np.gradient(ripple, axis=1)
                              * side / 64.0, 0, 255).astype(np.uint8)
    normal[:, :, 1] = np.clip(128.0 + 6.0 * np.gradient(ripple, axis=0)
                              * side / 64.0, 0, 255).astype(np.uint8)
    normal[:, :, 2] = 255
    Image.fromarray(normal, mode="RGB").save(
        args.output / "water_normal.png")

    pbr = np.zeros((side, side, 4), dtype=np.uint8)
    pbr[:, :, 0] = 20
    pbr[:, :, 1] = 0
    pbr[:, :, 2] = 255
    pbr[:, :, 3] = 255
    Image.fromarray(pbr, mode="RGBA").save(args.output / "water_pbr.png")

    height = (np.clip(ripple * 0.02, 0.0, 1.0) * 65535.0).astype(np.uint16)
    Image.fromarray(height).save(
        args.output / "water_height16.png")
    print(args.output)


if __name__ == "__main__":
    main()
