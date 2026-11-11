"""Convert exported FX EXR files to 8-bit PNG and write VL texture descriptors."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import OpenEXR
from PIL import Image


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
FX_ROOT = RESOURCE_ROOT / "Common/Source/UE/NiagaraExamples/Textures/FX"
DESCRIPTOR_ROOT = RESOURCE_ROOT / "Common/Textures/UE_FX"
MANIFEST_DIR = RESOURCE_ROOT / "Generated/Import/ue_niagara_assets"


def read_exr(path: Path) -> np.ndarray:
    parts = OpenEXR.File(str(path)).parts
    if not parts:
        raise RuntimeError("EXR has no parts: " + str(path))
    channels = parts[0].channels
    if "RGBA" in channels:
        rgba = channels["RGBA"].pixels.astype(np.float32)
        return rgba
    height, width = None, None
    values = {}
    for name, channel in channels.items():
        data = channel.pixels.astype(np.float32)
        if data.ndim == 2:
            height, width = data.shape
        elif data.ndim == 3:
            height, width = data.shape[:2]
        values[name] = data
    if height is None or width is None:
        raise RuntimeError("cannot determine EXR size: " + str(path))
    if "Y" in values and "R" not in values:
        grey = values["Y"]
        if grey.ndim == 3:
            grey = grey[..., 0]
        rgba = np.stack([grey, grey, grey, np.ones_like(grey)], axis=-1)
    else:
        planes = []
        for name in ("R", "G", "B"):
            if name not in values:
                raise RuntimeError("missing EXR channel %s: %s" % (name, path))
            plane = values[name]
            if plane.ndim == 3:
                plane = plane[..., 0]
            planes.append(plane)
        alpha = values.get("A")
        if alpha is None:
            alpha = np.ones_like(planes[0])
        elif alpha.ndim == 3:
            alpha = alpha[..., 0]
        rgba = np.stack(planes + [alpha], axis=-1)
    return rgba


def conversion_manifest() -> None:
    DESCRIPTOR_ROOT.mkdir(parents=True, exist_ok=True)
    MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
    records = []
    for exr_path in sorted(FX_ROOT.rglob("*.exr")):
        png_path = exr_path.with_suffix(".png")
        try:
            rgba = read_exr(exr_path)
            rgba = np.nan_to_num(rgba, nan=0.0, posinf=1.0, neginf=0.0)
            rgba = np.clip(rgba, 0.0, 1.0)
            image = Image.fromarray((rgba * 255.0 + 0.5).astype(np.uint8), "RGBA")
            image.save(png_path, optimize=True)
            records.append({
                "name": exr_path.stem,
                "exr": str(exr_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
                "png": str(png_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
                "size": list(image.size),
                "min": [float(value) for value in rgba.min(axis=(0, 1))],
                "max": [float(value) for value in rgba.max(axis=(0, 1))],
                "mean": [float(value) for value in rgba.mean(axis=(0, 1))],
            })
        except Exception as error:
            records.append({
                "name": exr_path.stem,
                "exr": str(exr_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
                "error": str(error),
            })

    descriptors = []
    for png_path in sorted(FX_ROOT.rglob("*.png")):
        name = png_path.stem
        image = Image.open(png_path)
        if image.mode == "L":
            image = image.convert("RGBA")
            image.save(png_path, optimize=True)
        elif image.mode != "RGBA":
            image = image.convert("RGBA")
            image.save(png_path, optimize=True)

        lower = name.lower()
        is_normal = "normal" in lower or lower.endswith("_n")
        is_orm = lower.endswith("_orm") or lower.endswith("_mr")
        is_dpr = lower.endswith("_dpr")
        is_mask = "mask" in lower
        is_color = (not is_normal and not is_orm and not is_dpr
                    and not lower.endswith("_noise")
                    and "noise" not in lower and not lower.startswith("t_hermite"))
        if lower.startswith("t_hermite") or "noise" in lower:
            color_space = "linear"
        elif is_normal or is_orm or is_dpr:
            color_space = "linear"
        elif is_mask:
            color_space = "sRGB"
        else:
            color_space = "sRGB"

        if is_normal:
            channels = {
                "r": "normalX", "g": "normalY", "b": "normalZ", "a": "unused"}
        elif is_orm:
            channels = {
                "r": "roughness", "g": "metallic", "b": "ao", "a": "unused"}
        elif is_dpr:
            channels = {
                "r": "baseColor", "g": "baseColor", "b": "baseColor",
                "a": "roughness"}
        elif is_mask:
            channels = {
                "r": "coverage", "g": "coverage", "b": "coverage",
                "a": "coverage"}
        else:
            channels = {
                "r": "red", "g": "green", "b": "blue", "a": "coverage"}

        descriptor = {
            "name": name,
            "type": "texture",
            "source": str(png_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
            "colorSpace": color_space,
            "mipmaps": True,
            "filter": "linear",
            "wrapMode": "clamp",
            "channelsDescription": channels,
        }
        descriptor_path = DESCRIPTOR_ROOT / (name + ".json")
        descriptor_path.write_text(
            json.dumps(descriptor, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")
        descriptors.append({
            "name": name,
            "descriptor": str(descriptor_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
            "source": descriptor["source"],
            "colorSpace": color_space,
            "size": list(image.size),
        })

    manifest = {
        "converted": records,
        "descriptors": descriptors,
    }
    manifest_path = MANIFEST_DIR / "fx_texture_build_manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")
    print("converted=%d descriptors=%d" % (len(records), len(descriptors)))


if __name__ == "__main__":
    conversion_manifest()
