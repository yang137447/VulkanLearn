import argparse
import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector


OBJECT_NAMES = (
    "BL",
    "FR",
    "FL",
    "BR",
)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    parser.add_argument(
        "--manifest",
        default="Maps/SC_car_showcase/Source/Models/car/car_asset_manifest.json",
    )
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def load_json(path):
    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def blender_to_engine_vector(value):
    return Vector((value.x, value.z, -value.y))


def load_obj_loop_normals(path):
    normals = []
    loop_normals = []
    with path.open("r", encoding="utf-8") as input_file:
        for line in input_file:
            if line.startswith("vn "):
                fields = line.split()
                normals.append(
                    Vector(
                        (
                            float(fields[1]),
                            float(fields[2]),
                            float(fields[3]),
                        )
                    )
                )
            elif line.startswith("f "):
                for corner in line.split()[1:]:
                    normal_index = int(corner.split("/")[-1])
                    if normal_index < 0:
                        normal_index = (
                            len(normals) + normal_index
                        )
                    else:
                        normal_index -= 1
                    loop_normals.append(
                        normals[normal_index]
                    )

    return loop_normals


def compare_vectors(left, right):
    return (left.normalized() - right.normalized()).length


def main():
    arguments = parse_arguments()
    resource_root = Path(arguments.resource_root)
    manifest = load_json(resource_root / arguments.manifest)
    parts_by_object = {
        part["blenderObject"]: part
        for part in manifest["parts"]
    }

    results = []
    for object_name in OBJECT_NAMES:
        source_object = bpy.data.objects.get(object_name)
        if source_object is None:
            raise RuntimeError(
                "Missing Blender wheel object: "
                + object_name
            )
        part = parts_by_object.get(object_name)
        if part is None:
            raise RuntimeError(
                "Car manifest has no wheel entry: "
                + object_name
            )
        model_json = load_json(resource_root / part["modelPath"])
        obj_path = resource_root / model_json["modelDataPath"]
        obj_normals = load_obj_loop_normals(obj_path)
        blender_normals = [
            blender_to_engine_vector(
                loop.normal.normalized()
            )
            for loop in source_object.data.loops
        ]
        paired_errors = [
            compare_vectors(blender_normal, obj_normal)
            for blender_normal, obj_normal in zip(
                blender_normals,
                obj_normals,
            )
        ]
        inverted_paired_errors = [
            compare_vectors(blender_normal, -obj_normal)
            for blender_normal, obj_normal in zip(
                blender_normals,
                obj_normals,
            )
        ]

        results.append(
            {
                "blenderObject": object_name,
                "modelPath": part["modelPath"],
                "blenderLoopCount": len(blender_normals),
                "objLoopNormalCount": len(obj_normals),
                "loopCountDifference": abs(
                    len(blender_normals) - len(obj_normals)
                ),
                "directMaxError": (
                    max(paired_errors)
                    if paired_errors
                    else None
                ),
                "directMeanError": (
                    sum(paired_errors) / len(paired_errors)
                    if paired_errors
                    else None
                ),
                "invertedMaxError": (
                    max(inverted_paired_errors)
                    if inverted_paired_errors
                    else None
                ),
                "invertedMeanError": (
                    sum(inverted_paired_errors)
                    / len(inverted_paired_errors)
                    if inverted_paired_errors
                    else None
                ),
                "firstBlenderNormal": list(blender_normals[0])
                if blender_normals
                else None,
                "firstObjNormal": list(obj_normals[0])
                if obj_normals
                else None,
            }
        )

    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
