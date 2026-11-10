"""Pack the project's baked landscape maps into the two runtime globals.

Outputs (under ``--output``):

    Source/T_global_colour.png   2048^2 RGB   (macro albedo, sRGB)
    Source/T_global_masks.png    2048^2 RGBA  (R = AO, G = wetness, linear)
    T_global_colour.json / T_global_masks.json

The project's baked maps are rotated 270 degrees counter-clockwise relative to
our heightmap (pinned by correlating AO against the heightmap cavity), so the
rotation is applied here once; the shader samples them without flipping.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def _load(path: Path, mode: str) -> Image.Image:
    image = Image.open(path)
    if mode == "rgb":
        return image.convert("RGB")
    return image


def _scalar(image: Image.Image, size: int) -> np.ndarray:
    if image.mode.startswith("I;16") or image.mode == "I":
        values = np.asarray(image, dtype=np.float32) / 65535.0
    else:
        values = np.asarray(image.convert("L"), dtype=np.float32) / 255.0
    if values.shape != (size, size):
        values = np.asarray(
            Image.fromarray((np.clip(values, 0, 1) * 65535).astype(np.uint16))
            .resize((size, size), Image.Resampling.BOX),
            dtype=np.float32) / 65535.0
    return values


def _rotate(image: Image.Image, degrees: int) -> Image.Image:
    if degrees % 360 == 0:
        return image
    return image.rotate(degrees, expand=True,
                        resample=Image.Resampling.BILINEAR)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--colour", type=Path, required=True)
    parser.add_argument("--ao", type=Path, required=True)
    parser.add_argument("--wetness", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--rotation", type=int, default=270)
    args = parser.parse_args()

    output = args.output.resolve()
    source = output / "Source"
    source.mkdir(parents=True, exist_ok=True)
    size = args.size

    colour = _rotate(_load(args.colour, "rgb"), args.rotation)
    if colour.size != (size, size):
        colour = colour.resize((size, size), Image.Resampling.LANCZOS)
    colour.save(source / "T_global_colour.png")

    ao = _scalar(_rotate(_load(args.ao, "scalar"), args.rotation), size)
    wet = _scalar(_rotate(_load(args.wetness, "scalar"), args.rotation), size)
    masks = np.zeros((size, size, 4), dtype=np.uint8)
    masks[:, :, 0] = np.clip(ao * 255.0, 0, 255).astype(np.uint8)
    masks[:, :, 1] = np.clip(wet * 255.0, 0, 255).astype(np.uint8)
    masks[:, :, 2] = 255
    masks[:, :, 3] = 255
    Image.fromarray(masks, mode="RGBA").save(source / "T_global_masks.png")

    for name, source_name, color_space in (
            ("T_global_colour", "Source/T_global_colour.png", "srgb"),
            ("T_global_masks", "Source/T_global_masks.png", "linear")):
        (output / f"{name}.json").write_text(
            json.dumps({
                "name": name,
                "type": "texture",
                "source": f"Generated/HoudiniPegasusIsland/Global/{source_name}",
                "colorSpace": color_space,
                "mipmaps": True,
                "filter": "linear",
                "wrapMode": "clamp",
            }, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")

    print(json.dumps({
        "colour": str(source / "T_global_colour.png"),
        "masks": str(source / "T_global_masks.png"),
        "aoMean": round(float(ao.mean()), 4),
        "wetMean": round(float(wet.mean()), 4),
        "rotation": args.rotation,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
