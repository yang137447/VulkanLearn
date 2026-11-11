"""Download the five Poly Haven terrain layers and stage them for packing.

Poly Haven is CC0 and ships 16-bit displacement as EXR (or 16-bit PNG), which
matches the terrain texture spec:
``documents/plan/rendering/terrain-texture-packing.md``.

Output layout (``--out``):

    src/<key>_A.png   diffuse
    src/<key>_N.png   nor_dx  (DirectX green, same as the UE convention)
    src/<key>_R.png   roughness
    src/<key>_D.png   16-bit displacement (converted from EXR)

Then run::

    python build_megascans_layers.py --source <out>/src --output <...> \
        --no-tint --material gravel=gravel ... 
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ASSETS = {
    "gravel": "aerial_grass_rock",
    "rock": "rock_face_03",
    "debris": "aerial_rocks_02",
    "silt": "brown_mud_dry",
    "sand": "aerial_beach_01",
}

MAPS = (
    ("diff", "Diffuse", "png"),
    ("nor", "nor_dx", "png"),
    ("rough", "Rough", "png"),
    ("disp", "Displacement", "exr"),
)

USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"


def _get_json(url: str) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


def _download(url: str, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=600) as response, \
            open(path, "wb") as stream:
        shutil.copyfileobj(response, stream, length=1 << 20)


def _convert_exr(blender: Path, script: Path, source: Path, target: Path) -> None:
    subprocess.run([
        str(blender), "--background", "--factory-startup",
        "--python", str(script), "--", str(source), str(target),
    ], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--resolution", default="4k")
    parser.add_argument("--blender", type=Path, default=None)
    parser.add_argument("--exr-script", type=Path, default=None)
    args = parser.parse_args()

    out = args.out.resolve()
    raw = out / "raw"
    staged = out / "src"
    staged.mkdir(parents=True, exist_ok=True)

    for key, asset in ASSETS.items():
        files = _get_json(f"https://api.polyhaven.com/files/{asset}")
        for short, map_name, fmt in MAPS:
            per_resolution = files.get(map_name, {})
            entry = per_resolution.get(args.resolution) or per_resolution.get("2k")
            if not entry:
                raise RuntimeError(f"Poly Haven asset {asset} has no {map_name}")
            info = entry.get(fmt) or entry.get("png") or entry.get("jpg")
            if not info:
                raise RuntimeError(f"Poly Haven asset {asset} {map_name} has no {fmt}")
            target = raw / key / os.path.basename(info["url"])
            if not target.is_file() or target.stat().st_size != info.get("size"):
                print(f"GET {asset} {map_name} -> {target.name}")
                _download(info["url"], target)
            suffix = {"diff": "A", "nor": "N", "rough": "R", "disp": "D"}[short]
            staged_path = staged / f"{key}_{suffix}.png"
            if short == "disp":
                if args.blender is None or args.exr_script is None:
                    raise RuntimeError(
                        "--blender and --exr-script are required for 16-bit "
                        "displacement conversion")
                _convert_exr(args.blender, args.exr_script, target, staged_path)
            else:
                shutil.copyfile(target, staged_path)
            print(f"STAGE {staged_path.name}")
    print("src", staged)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
