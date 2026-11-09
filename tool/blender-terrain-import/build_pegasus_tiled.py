"""Build the Pegasus global terrain, split it into 4x4 tiles, and assemble a scene."""

from __future__ import annotations

import argparse
import json
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
    parser.add_argument("--lod-levels", type=int, default=3)
    parser.add_argument(
        "--lod-distances-m", type=float, nargs=2, default=(1250.0, 2500.0))
    parser.add_argument("--tile-size-m", type=float, default=1000.0)
    parser.add_argument("--page-tiles-per-side", type=int, default=8)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument(
        "--macro-island", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--spine-strength", type=float, default=0.16)
    parser.add_argument("--noise-amplitude", type=float, default=None)
    parser.add_argument("--noise-element", type=float, default=None)
    parser.add_argument("--detail-amplitude", type=float, default=None)
    parser.add_argument("--detail-element", type=float, default=None)
    parser.add_argument("--noise-distortion", type=float, default=None)
    parser.add_argument("--rhythm-strength", type=float, default=None)
    parser.add_argument("--rhythm-element", type=float, default=None)
    parser.add_argument(
        "--erodibility", type=float, default=1.0e-4,
        help="Pegasus visual calibration; erode_pegasus_global.py's LEM "
             "default is 3e-6")
    parser.add_argument("--dt-years", type=float, default=1000.0)
    parser.add_argument("--steps", type=int, default=12)
    parser.add_argument("--chain", choices=("single", "pegasus"),
                        default="single")
    parser.add_argument("--chain-k", type=float, default=1.0)
    parser.add_argument(
        "--chain-resample", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument(
        "--chain-warp", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--chain-warp-mask", type=float, default=0.75)
    parser.add_argument("--chain-detail-restore", type=float, default=0.0)
    parser.add_argument("--warp-seed", type=int, default=17)
    parser.add_argument(
        "--river", action=argparse.BooleanOptionalAction, default=True,
        help="run the simplified PE_heightfield_paths river carve")
    parser.add_argument("--river-width-m", type=float, default=110.0)
    parser.add_argument(
        "--river-water-width-m", type=float, default=None,
        help="水面核心宽度；省略时 river_paths 取 width-m × 0.55")
    parser.add_argument("--river-depth-m", type=float, default=6.0)
    parser.add_argument("--river-bank-m", type=float, default=160.0)
    parser.add_argument("--river-smooth", type=int, default=2)
    parser.add_argument("--river-branches", type=int, default=3)
    parser.add_argument("--river-waypoints", type=Path, default=None)
    parser.add_argument("--water-texture-root", type=Path, default=None)
    parser.add_argument("--passes", choices=("one", "two"), default="one")
    parser.add_argument("--skip-erode", action="store_true")
    parser.add_argument("--reuse-global", action="store_true")
    parser.add_argument("--split-only", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def _run(command: list[str], verbose: bool) -> None:
    if verbose:
        print("> " + " ".join(str(item) for item in command))
    subprocess.run(command, check=True)


def _append_derived_layers(manifest_path: Path, source_dir: Path,
                           layer_texture_root: Path,
                           water_texture_root: Path | None) -> None:
    """Append S4 debris/sediment layers to the global manifest.

    The local Megascans library is UE-uasset-only, so these first-pass
    layers use KiteDemo surface textures as placeholders. The weight maps
    are the real S4 masks from the erosion stage.
    """
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    layers = manifest.setdefault("layers", [])
    existing = {layer.get("name") for layer in layers}
    specs = (
        ("Scree Debris", "mask_gn_debris.png", "layer_05", 5.0),
        ("River Silt", "mask_gn_sediment.png", "layer_01", 6.0),
    )
    for name, mask_name, prefix, uv_scale in specs:
        if name in existing:
            continue
        mask_path = (source_dir / mask_name).resolve()
        if not mask_path.is_file():
            continue
        layers.append({
            "name": name,
            "weightmapPath": str(mask_path),
            "textures": {
                "albedo": str((layer_texture_root / f"{prefix}_albedo.png").resolve()),
                "normal": str((layer_texture_root / f"{prefix}_normal.png").resolve()),
                "pbr": str((layer_texture_root / f"{prefix}_pbr.png").resolve()),
                "height": str((layer_texture_root / f"{prefix}_height16.png").resolve()),
            },
            "heightRange": [0.0, 1.0],
            "slopeRange": [0.0, 90.0],
            "uvScale": uv_scale,
            "uvRotation": 0.0,
            "noise": 0.03,
        })
    bank_mask = (source_dir / "mask_gn_river.png").resolve()
    if bank_mask.is_file() and "River Bank" not in existing:
        layers.append({
            "name": "River Bank",
            "weightmapPath": str(bank_mask),
            "textures": {
                "albedo": str((layer_texture_root / "layer_06_albedo.png").resolve()),
                "normal": str((layer_texture_root / "layer_06_normal.png").resolve()),
                "pbr": str((layer_texture_root / "layer_06_pbr.png").resolve()),
                "height": str((layer_texture_root / "layer_06_height16.png").resolve()),
            },
            "heightRange": [0.0, 1.0],
            "slopeRange": [0.0, 90.0],
            "uvScale": 5.5,
            "uvRotation": 0.0,
            "noise": 0.03,
        })
    channel_mask = (source_dir / "mask_gn_river_water.png").resolve()
    if (water_texture_root is not None and channel_mask.is_file()
            and "River Water" not in existing):
        layers.append({
            "name": "River Water",
            "weightmapPath": str(channel_mask),
            "textures": {
                "albedo": str((water_texture_root / "water_albedo.png").resolve()),
                "normal": str((water_texture_root / "water_normal.png").resolve()),
                "pbr": str((water_texture_root / "water_pbr.png").resolve()),
                "height": str((water_texture_root / "water_height16.png").resolve()),
            },
            "heightRange": [0.0, 1.0],
            "slopeRange": [0.0, 90.0],
            "uvScale": 4.0,
            "uvRotation": 0.0,
            "noise": 0.03,
        })
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


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
    water_texture_root = (
        args.water_texture_root.resolve()
        if args.water_texture_root is not None
        else resource_root / "Generated/PegasusWater/Textures/Source"
    )
    if args.tile_grid < 2 or args.tile_grid * args.page_tiles_per_side > 32:
        raise RuntimeError(
            "tile-grid * page-tiles-per-side must fit the 32x32 page table")

    global_source.mkdir(parents=True, exist_ok=True)
    if not args.reuse_global:
        shape_command = [
            str(args.blender), "--background", "--python",
            str(repo / "tool/blender-terrain-import/build_pegasus_library_global.py"),
            "--",
            "--output-dir", str(global_source),
            "--manifest", str(global_manifest),
            "--layer-texture-root", str(layer_texture_root),
            "--resolution", str(args.global_resolution),
            "--size-m", str(args.tile_grid * args.tile_size_m),
            "--seed", str(args.seed),
            "--macro-island" if args.macro_island else "--no-macro-island",
            "--spine-strength", str(args.spine_strength),
            "--no-erosion",
        ]
        for name, value in (
                ("--noise-amplitude", args.noise_amplitude),
                ("--noise-element", args.noise_element),
                ("--detail-amplitude", args.detail_amplitude),
                ("--detail-element", args.detail_element),
                ("--noise-distortion", args.noise_distortion),
                ("--rhythm-strength", args.rhythm_strength),
                ("--rhythm-element", args.rhythm_element)):
            if value is not None:
                shape_command.extend([name, str(value)])
        _run(shape_command, args.verbose)
        if not args.skip_erode:
            _run([
                str(args.blender_python),
                str(repo / "tool/blender-terrain-import/erode_pegasus_global.py"),
                "--",
                "--manifest", str(global_manifest),
                "--input", str(global_source / "heightmap_shape.png"),
                "--cache-dir", str(global_root / "Cache"),
                "--size-m", str(args.tile_grid * args.tile_size_m),
                "--erodibility", str(args.erodibility),
                "--dt-years", str(args.dt_years),
                "--steps", str(args.steps),
                "--chain", str(args.chain),
                "--chain-k", str(args.chain_k),
                "--chain-resample" if args.chain_resample
                else "--no-chain-resample",
                "--chain-warp" if args.chain_warp else "--no-chain-warp",
                "--chain-warp-mask", str(args.chain_warp_mask),
                "--chain-detail-restore", str(args.chain_detail_restore),
                "--warp-seed", str(args.warp_seed),
                "--passes", str(args.passes),
            ], args.verbose)
        if args.river:
            # Houdini 的 PE_heightfield_paths 位于侵蚀网络**下游**，消费的是
            # 侵蚀后的高度场，本身只产出 mask 层。放在侵蚀之后再切河床，
            # 河道剖面才不会被侵蚀抹平。
            source_height = (
                global_source / "heightmap_shape.png"
                if args.skip_erode
                else global_source / "heightmap_gn.png")
            manifest = json.loads(
                global_manifest.read_text(encoding="utf-8-sig"))
            river_meta = global_source / "heightmap_river_input.json"
            river_meta.write_text(
                json.dumps({
                    "low": float(manifest["sourceLow"]),
                    "high": float(manifest["sourceHigh"]),
                }, indent=2) + "\n",
                encoding="utf-8")
            river_output = global_source / "heightmap_river.png"
            _run([
                str(args.blender_python),
                str(repo / "tool/blender-terrain-import/river_paths.py"),
                "--heightmap", str(source_height),
                "--meta", str(river_meta),
                "--output-heightmap", str(river_output),
                "--output-meta", str(global_source / "heightmap_river.json"),
                "--output-mask", str(global_source / "mask_gn_river.png"),
                "--output-water-mask",
                str(global_source / "mask_gn_river_water.png"),
                "--output-path", str(global_source / "river_path.json"),
                "--size-m", str(args.tile_grid * args.tile_size_m),
                "--width-m", str(args.river_width_m),
                "--depth-m", str(args.river_depth_m),
                "--bank-m", str(args.river_bank_m),
                "--smooth-iterations", str(args.river_smooth),
                "--branch-count", str(args.river_branches),
            ] + (["--waypoints", str(args.river_waypoints.resolve())]
                 if args.river_waypoints is not None else [])
              + (["--water-width-m", str(args.river_water_width_m)]
                 if args.river_water_width_m is not None else []),
            args.verbose)
            carved_meta = json.loads(
                (global_source / "heightmap_river.json").read_text(
                    encoding="utf-8"))
            manifest["heightmapPath"] = str(river_output.resolve())
            manifest["sourceLow"] = float(carved_meta["low"])
            manifest["sourceHigh"] = float(carved_meta["high"])
            global_manifest.write_text(
                json.dumps(manifest, ensure_ascii=False, indent=4) + "\n",
                encoding="utf-8")
    elif not global_manifest.is_file():
        raise RuntimeError(f"missing reusable global manifest: {global_manifest}")

    _append_derived_layers(global_manifest, global_source, layer_texture_root,
                           water_texture_root)

    _run([
        str(args.blender_python),
        str(repo / "tool/blender-terrain-import/split_pegasus_tiles.py"),
        "--source-manifest", str(global_manifest),
        "--resource-root", str(resource_root),
        "--output-root", str(output_root),
        "--tile-grid", str(args.tile_grid),
        "--tile-resolution", str(args.tile_resolution),
        "--heightmap-margin", str(args.heightmap_margin),
        "--lod-levels", str(args.lod_levels),
        "--lod-distances-m", str(args.lod_distances_m[0]),
        str(args.lod_distances_m[1]),
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
