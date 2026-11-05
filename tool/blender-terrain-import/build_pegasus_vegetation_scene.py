"""Build a Pegasus vegetation slice from extracted prototypes and scatter points.

The cypress GLB is produced separately by ``extract_pegasus_vegetation.py``
(inside Blender).  This script is pure Python and does the deterministic scene
assembly:

    terrain scene + scatter_points.json
      -> SM_pegasus_cypress.json
      -> SC_terrain_pegasus_cypress_grove.json

It can also emit a scene-local TR/MI pair with a finer ``u_uvScale`` so the
base Pegasus terrain asset is not modified.
"""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build Pegasus vegetation scene")
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument(
        "--terrain-scene",
        default="Generated/BlenderPegasusGN/SC_terrain_blender_gn_pegasus.json",
    )
    parser.add_argument(
        "--scatter",
        default="Generated/BlenderPegasusGN/Source/scatter_points.json",
    )
    parser.add_argument(
        "--tree-glb",
        default="Generated/BlenderPegasusGN/Vegetation/cypress.glb",
    )
    parser.add_argument(
        "--out-scene",
        default="Generated/BlenderPegasusGN/SC_terrain_pegasus_cypress_grove.json",
    )
    parser.add_argument(
        "--out-mesh",
        default="Generated/BlenderPegasusGN/Vegetation/SM_pegasus_cypress.json",
    )
    parser.add_argument("--count", type=int, default=122)
    parser.add_argument(
        "--area",
        type=float,
        nargs=4,
        default=(-12000.0, 12000.0, 165000.0, 188000.0),
        metavar=("X_MIN", "X_MAX", "Z_MIN", "Z_MAX"),
    )
    parser.add_argument(
        "--camera-position", type=float, nargs=3, default=(0.0, 30000.0, 190000.0)
    )
    parser.add_argument(
        "--look-at", type=float, nargs=3, default=(0.0, 30000.0, 165000.0)
    )
    parser.add_argument("--fov", type=float, default=60.0)
    parser.add_argument("--scale-min", type=float, default=180.0)
    parser.add_argument("--scale-max", type=float, default=260.0)
    parser.add_argument("--seed", type=int, default=20261105)
    parser.add_argument(
        "--uv-scale",
        type=float,
        default=128.0,
        help="scene-local terrain MI u_uvScale; <=0 keeps the source MI",
    )
    return parser.parse_args()


def _resource_path(root: Path, relative: str) -> Path:
    return root / relative


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    args = _parse_args()
    root = args.resource_root.resolve()
    terrain_scene_path = _resource_path(root, str(args.terrain_scene))
    scene = json.loads(terrain_scene_path.read_text(encoding="utf-8"))
    scatter = json.loads(
        _resource_path(root, str(args.scatter)).read_text(encoding="utf-8")
    )

    x_min, x_max, z_min, z_max = args.area
    candidates = [
        point
        for point in scatter.get("points", [])
        if x_min <= point["position"][0] <= x_max
        and z_min <= point["position"][2] <= z_max
    ]
    if not candidates:
        raise RuntimeError("scatter area contains no points")
    rng = random.Random(args.seed)
    picked = rng.sample(candidates, min(args.count, len(candidates)))

    mesh_relative = str(args.out_mesh)
    mesh_absolute = _resource_path(root, mesh_relative)
    if not _resource_path(root, str(args.tree_glb)).is_file():
        raise RuntimeError(
            f"missing extracted cypress GLB: {args.tree_glb}; run "
            "extract_pegasus_vegetation.py first"
        )
    _write_json(
        mesh_absolute,
        {
            "name": "Pegasus Cypress",
            "type": "mesh",
            "modelDataPath": str(args.tree_glb),
            "materialSlots": [
                {
                    "name": "Foliage_Leaves.DoubleSided",
                    "materialInstancePath": (
                        "Maps/SC_bistro_exterior_modular/Materials/"
                        "MI_Foliage_Leaves.json"
                    ),
                },
                {
                    "name": "Foliage_Trunk",
                    "materialInstancePath": (
                        "Maps/SC_bistro_exterior_modular/Materials/"
                        "MI_Foliage_Trunk.json"
                    ),
                },
            ],
        },
    )

    terrain_object = next(
        (obj for obj in scene.get("objects", []) if obj.get("type") == "terrain"),
        None,
    )
    if terrain_object is None:
        raise RuntimeError("terrain scene has no terrain object")

    if args.uv_scale > 0.0:
        source_tr_path = _resource_path(root, terrain_object["terrainPath"])
        source_tr = json.loads(source_tr_path.read_text(encoding="utf-8"))
        source_mi_path = _resource_path(root, source_tr["materialInstancePath"])
        source_mi = json.loads(source_mi_path.read_text(encoding="utf-8"))
        source_mi.setdefault("parameters", {})["u_uvScale"] = args.uv_scale
        source_mi["name"] = str(source_mi.get("name", "MI_terrain")) + "_grove"
        grove_mi_relative = str(
            Path(terrain_object["terrainPath"]).parent
            / (Path(terrain_object["terrainPath"]).stem + "_grove_mi.json")
        )
        _write_json(_resource_path(root, grove_mi_relative), source_mi)
        source_tr["name"] = str(source_tr.get("name", "TR_terrain")) + "_grove"
        source_tr["materialInstancePath"] = grove_mi_relative
        grove_tr_relative = str(
            Path(terrain_object["terrainPath"]).parent
            / (Path(terrain_object["terrainPath"]).stem + "_grove.json")
        )
        _write_json(_resource_path(root, grove_tr_relative), source_tr)
        terrain_object["terrainPath"] = grove_tr_relative

    for obj in scene.get("objects", []):
        if obj.get("type") != "camera":
            continue
        obj["position"] = [float(value) for value in args.camera_position]
        obj["look_at"] = [float(value) for value in args.look_at]
        obj["rotation"] = [0.0, 0.0, 0.0]
        obj["fov"] = args.fov
        obj["near_clip"] = 10.0
        obj["far_clip"] = 600000.0
        obj.setdefault("scale", [1.0, 1.0, 1.0])

    scene["name"] = "SC_terrain_pegasus_cypress_grove"
    scene["description"] = (
        "Pegasus 复刻切片：从 Bistro FillOut 资产抽出的 Italian Cypress 原型，"
        "按 Blender GN scatter 点确定性落位；当前逐对象 draw，实例化是后续工作。"
    )
    for index, point in enumerate(picked):
        scale = rng.uniform(args.scale_min, args.scale_max)
        scene["objects"].append(
            {
                "name": f"Cypress_{index:03d}",
                "type": "mesh",
                "modelPath": mesh_relative,
                "position": [float(value) for value in point["position"]],
                "rotation": [0.0, rng.uniform(0.0, 360.0), 0.0],
                "scale": [scale, scale, scale],
            }
        )

    _write_json(_resource_path(root, str(args.out_scene)), scene)
    print(
        json.dumps(
            {
                "scene": str(args.out_scene),
                "mesh": mesh_relative,
                "trees": len(picked),
                "candidates": len(candidates),
                "uvScale": args.uv_scale,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
