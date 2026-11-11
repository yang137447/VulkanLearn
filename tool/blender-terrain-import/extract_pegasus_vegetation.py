"""Extract a single vegetation prototype from a licensed source GLB.

The Bistro FillOut_BushTree asset is a whole street fill-out cluster.  This
script keeps only the Italian cypress meshes, recenters them to the origin, and
exports a compact GLB that can be consumed as a VulkanLearn SM_* prototype.

Run:

    blender --background --python extract_pegasus_vegetation.py -- \
        --source <FillOut_BushTree.glb> \
        --output <Generated/BlenderPegasusGN/Vegetation/cypress.glb> \
        --metadata <.../cypress.metadata.json>
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def _parse_args() -> argparse.Namespace:
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    parser = argparse.ArgumentParser(description="Extract a cypress prototype")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--match", default="Italian_Cypress")
    return parser.parse_args(argv)


def _world_bounds(objects: list[bpy.types.Object]) -> tuple[Vector, Vector]:
    minimum = Vector((float("inf"),) * 3)
    maximum = Vector((float("-inf"),) * 3)
    for obj in objects:
        if obj.type != "MESH":
            continue
        for corner in obj.bound_box:
            world = obj.matrix_world @ Vector(corner)
            minimum.x = min(minimum.x, world.x)
            minimum.y = min(minimum.y, world.y)
            minimum.z = min(minimum.z, world.z)
            maximum.x = max(maximum.x, world.x)
            maximum.y = max(maximum.y, world.y)
            maximum.z = max(maximum.z, world.z)
    return minimum, maximum


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(args.source))

    kept: list[bpy.types.Object] = []
    for obj in list(bpy.context.scene.objects):
        if args.match.casefold() in obj.name.casefold():
            kept.append(obj)
    if not kept:
        raise RuntimeError(f"No objects matched {args.match!r} in {args.source}")
    kept_meshes = [obj for obj in kept if obj.type == "MESH"]
    if not kept_meshes:
        raise RuntimeError("Matched objects contain no mesh data")

    # Keep the selected mesh objects, but detach them from the source hierarchy
    # so the exported file has no unrelated empty nodes.
    for obj in list(bpy.context.scene.objects):
        if obj not in kept_meshes:
            bpy.data.objects.remove(obj, do_unlink=True)
    for obj in kept_meshes:
        matrix = obj.matrix_world.copy()
        obj.parent = None
        obj.matrix_world = matrix

    minimum, maximum = _world_bounds(kept_meshes)
    center = (minimum + maximum) * 0.5
    for obj in kept_meshes:
        obj.location.x -= center.x
        obj.location.z -= center.z
        obj.location.y -= minimum.y
    bpy.context.view_layer.update()
    minimum, maximum = _world_bounds(kept_meshes)

    bpy.ops.object.select_all(action="DESELECT")
    for obj in kept_meshes:
        obj.select_set(True)
    if kept_meshes:
        bpy.context.view_layer.objects.active = kept_meshes[0]

    materials = sorted({
        slot.material.name
        for obj in kept_meshes
        for slot in obj.material_slots
        if slot.material is not None
    })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(args.output),
        export_format="GLB",
        use_selection=True,
        export_apply=True,
    )

    metadata = {
        "source": str(args.source),
        "match": args.match,
        "objects": [obj.name for obj in kept_meshes],
        "materials": materials,
        "boundsMinMeters": [float(value) for value in minimum],
        "boundsMaxMeters": [float(value) for value in maximum],
        "sizeMeters": [
            float(maximum.x - minimum.x),
            float(maximum.y - minimum.y),
            float(maximum.z - minimum.z),
        ],
    }
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.write_text(
        json.dumps(metadata, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metadata, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
