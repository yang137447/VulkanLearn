#!/usr/bin/env python3
"""Inventory extracted 3d66 textures and report byte-identical duplicates."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path

from PIL import Image


IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr", ".bmp"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--texture-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    files = sorted(
        path
        for path in args.texture_dir.iterdir()
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )
    if not files:
        raise SystemExit(f"no image files in {args.texture_dir}")

    first_by_hash: dict[str, str] = {}
    rows = []
    for path in files:
        digest = sha256(path)
        duplicate_of = first_by_hash.setdefault(digest, path.name)
        with Image.open(path) as image:
            width, height = image.size
            image_format = image.format or path.suffix.lstrip(".").upper()
            mode = image.mode
            has_alpha = "A" in image.getbands()
        rows.append(
            {
                "source_file": path.name,
                "width": width,
                "height": height,
                "format": image_format,
                "mode": mode,
                "has_alpha": str(has_alpha).lower(),
                "bytes": path.stat().st_size,
                "sha256": digest,
                "duplicate_of": "" if duplicate_of == path.name else duplicate_of,
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    duplicates = sum(1 for row in rows if row["duplicate_of"])
    print(
        f"inventoried {len(rows)} textures, "
        f"{len(rows) - duplicates} unique, {duplicates} byte-identical duplicates"
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
