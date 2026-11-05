"""Build the Pegasus global terrain, split it into 4x4 tiles, and assemble a scene."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a Pegasus 4x4 PCG terrain scene")
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument(
        "--blender", type=Path,
        default=Path(r"D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\blender.exe"))
    parser.add_argument(
        "--blender-python", type=Path,
        default=Path(
            r"D:\YYBWorkSpace\Tools\blender-5.2.3-windows-x64\5.2\python\bin\python.exe"))
    parser.add_argument(
        "--layer-texture-root", type=Path,
        help="defaults to <resource-root>/Generated/KiteDemo/Textures/Source")
    parser.add_argument(
        "--output-relative", default="Generated/BlenderPegasusGN/Tiles")
    parser.add_argument("--global-resolution", type=int, default=1024)
    parser.add_argument("--tile-grid", type=int, default=4)
    parser.add_argument("--tile-resolution", type=int, default=256)
    parser.add_argument("--heightmap-margin", type=int, default=1)
    parser.add_argument("--tile-size-m", type=float, default=1000.0)
    parser.add_argument("--page-tiles-per-side", type=int, default=8)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--terrain-type", default="alpine")
    parser.add_argument("--scatter-density", type=float, default=0.0024)
    parser.add_argument("--reuse-global", action="store_true")
    parser.add_argument("--split-only", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def _run(command: list[str], verbose: bool) -> None:
    if verbose:
        print("> " + " ".join(str(item) for item in command))
    subprocess.run(command, check=True)


def main() -> int:
    args = _parse_args()
    repo = Path(__file__).resolve().parents[2]
    resource_root = args.resource_root.resolve()
    output_root = (resource_root / args.output_relative).resolve()
    global_root = output_root / "Global"
    global_source = global_root / "Source"
    global_manifest = global_root / "blender_gn_import.json"
    layer_texture_root = (
        args.layer_texture_root.resolve()
        if args.layer_texture_root is not None
        else resource_root / "Generated/KiteDemo/Textures/Source"
    )
    if args.tile_grid < 2 or args.tile_grid * args.page_tiles_per_side > 32:
        raise RuntimeError(
            "tile-grid * page-tiles-per-side must fit the 32x32 page table")

    global_source.mkdir(parents=True, exist_ok=True)
    if not args.reuse_global:
        _run([
            str(args.blender), "--background", "--python",
            str(repo / "tool/blender-terrain-import/build_pegasus_gn.py"), "--",
            "--output-dir", str(global_source),
            "--manifest", str(global_manifest),
            "--layer-texture-root", str(layer_texture_root),
            "--resolution", str(args.global_resolution),
            "--size-m", str(args.tile_grid * args.tile_size_m),
            "--seed", str(args.seed),
            "--terrain-type", str(args.terrain_type),
            "--scatter-density", str(args.scatter_density),
        ], args.verbose)
    elif not global_manifest.is_file():
        raise RuntimeError(f"missing reusable global manifest: {global_manifest}")

    _run([
        str(args.blender_python),
        str(repo / "tool/blender-terrain-import/split_pegasus_tiles.py"),
        "--source-manifest", str(global_manifest),
        "--resource-root", str(resource_root),
        "--output-root", str(output_root),
        "--tile-grid", str(args.tile_grid),
        "--tile-resolution", str(args.tile_resolution),
        "--heightmap-margin", str(args.heightmap_margin),
        "--tile-size-m", str(args.tile_size_m),
        "--page-tiles-per-side", str(args.page_tiles_per_side),
    ], args.verbose)

    if not args.split_only:
        assemble_command = [
            sys.executable,
            str(repo / "tool/blender-terrain-import/assemble_pegasus_tiles.py"),
            "--resource-root", str(resource_root),
            "--tile-layout", str(output_root / "tile_layout.json"),
        ]
        if args.verbose:
            assemble_command.append("--verbose")
        _run(assemble_command, args.verbose)
    print(str(output_root / "SC_terrain_pegasus_4x4.json"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
