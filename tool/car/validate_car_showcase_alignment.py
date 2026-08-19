import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    parser.add_argument(
        "--scene",
        default="scenes/SC_car_showcase.json",
    )
    parser.add_argument(
        "--manifest",
        default="models/datas/car/car_asset_manifest.json",
    )
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[
            sys.argv.index("--") + 1:
        ]
    return parser.parse_args(script_arguments)


def load_json(path):
    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def load_obj_positions(path):
    positions = []
    with path.open("r", encoding="utf-8") as input_file:
        for line in input_file:
            if not line.startswith("v "):
                continue
            fields = line.split()
            positions.append(
                Vector(
                    (
                        float(fields[1]),
                        float(fields[2]),
                        float(fields[3]),
                    )
                )
            )
    return positions


def convert_blender_point_to_engine(point):
    return Vector((point.x, point.z, -point.y))


def build_runtime_transform(scene_object):
    position = Vector(scene_object["position"])
    rotation = [
        math.radians(value)
        for value in scene_object["rotation"]
    ]
    scale = scene_object["scale"]

    # 与 CommonFunction::RotationToQuat 保持一致：Y * X * Z。
    rotation_matrix = (
        Matrix.Rotation(rotation[1], 4, "Y")
        @ Matrix.Rotation(rotation[0], 4, "X")
        @ Matrix.Rotation(rotation[2], 4, "Z")
    )
    scale_matrix = Matrix.Diagonal(
        Vector((scale[0], scale[1], scale[2], 1.0))
    )
    return (
        Matrix.Translation(position)
        @ rotation_matrix
        @ scale_matrix
    )


def calculate_bounds(points):
    minimum = Vector(
        (
            float("inf"),
            float("inf"),
            float("inf"),
        )
    )
    maximum = Vector(
        (
            float("-inf"),
            float("-inf"),
            float("-inf"),
        )
    )
    for point in points:
        for axis in range(3):
            minimum[axis] = min(minimum[axis], point[axis])
            maximum[axis] = max(maximum[axis], point[axis])
    return minimum, maximum


def max_vector_delta(left, right):
    return max(
        abs(left[axis] - right[axis])
        for axis in range(3)
    )


def vector_to_list(value):
    return [round(component, 6) for component in value]


def quantize_position(value):
    return (
        round(value.x, 4),
        round(value.y, 4),
        round(value.z, 4),
    )


def bounds_delta_sort_key(item):
    return (
        item["unmatchedPositionCount"],
        item["maximumBoundsDelta"],
    )


def build_wheel_summary(wheel_components):
    summaries = []
    for wheel_name in ("wheel_fl", "wheel_fr", "wheel_bl", "wheel_br"):
        points = wheel_components.get(wheel_name, [])
        if not points:
            continue
        minimum, maximum = calculate_bounds(points)
        summaries.append(
            {
                "wheel": wheel_name,
                # 车轮轴向与引擎 X 轴一致；宽度能直接区分前后轮是否被错误复用。
                "wheelWidth": round(
                    maximum.x - minimum.x,
                    6,
                ),
            }
        )
    return summaries


def main():
    arguments = parse_arguments()
    resource_root = Path(arguments.resource_root)
    scene_json = load_json(resource_root / arguments.scene)
    manifest_json = load_json(resource_root / arguments.manifest)

    scene_objects_by_model = {}
    for scene_object in scene_json["objects"]:
        model_path = scene_object.get("modelPath")
        if model_path:
            scene_objects_by_model[model_path] = scene_object

    results = []
    skipped = []
    wheel_components = {}
    for part in manifest_json["parts"]:
        scene_object = scene_objects_by_model.get(part["modelPath"])
        if scene_object is None:
            skipped.append(
                {
                    "blenderObject": part["blenderObject"],
                    "modelPath": part["modelPath"],
                }
            )
            continue

        source_object = bpy.data.objects.get(part["blenderObject"])
        if source_object is None:
            raise RuntimeError(
                "Missing Blender object: "
                + part["blenderObject"]
            )

        mesh_asset_json = load_json(
            resource_root / part["modelPath"]
        )
        obj_positions = load_obj_positions(
            resource_root / mesh_asset_json["modelDataPath"]
        )
        runtime_transform = build_runtime_transform(scene_object)

        source_world_positions = [
            convert_blender_point_to_engine(
                source_object.matrix_world @ vertex.co
            )
            for vertex in source_object.data.vertices
        ]
        if part["assetId"].startswith("wheel_"):
            wheel_components[part["assetId"]] = source_world_positions

        runtime_world_positions = [
            runtime_transform @ position
            for position in obj_positions
        ]
        maximum_vertex_delta = float("inf")
        if len(source_world_positions) == len(runtime_world_positions):
            maximum_vertex_delta = max(
                (
                    max_vector_delta(source_position, runtime_position)
                    for source_position, runtime_position in zip(
                        source_world_positions,
                        runtime_world_positions,
                    )
                ),
                default=0.0,
            )

        source_minimum, source_maximum = calculate_bounds(
            source_world_positions
        )
        runtime_minimum, runtime_maximum = calculate_bounds(
            runtime_world_positions
        )
        minimum_delta = max_vector_delta(
            source_minimum,
            runtime_minimum,
        )
        maximum_delta = max_vector_delta(
            source_maximum,
            runtime_maximum,
        )
        source_position_set = {
            quantize_position(position)
            for position in source_world_positions
        }
        runtime_position_set = {
            quantize_position(position)
            for position in runtime_world_positions
        }
        source_only_positions = (
            source_position_set - runtime_position_set
        )
        runtime_only_positions = (
            runtime_position_set - source_position_set
        )

        results.append(
            {
                "blenderObject": part["blenderObject"],
                "parent": part.get("parent"),
                "assetId": part["assetId"],
                "modelPath": part["modelPath"],
                "sourceMinimum": vector_to_list(source_minimum),
                "runtimeMinimum": vector_to_list(runtime_minimum),
                "sourceMaximum": vector_to_list(source_maximum),
                "runtimeMaximum": vector_to_list(runtime_maximum),
                "maximumBoundsDelta": max(
                    minimum_delta,
                    maximum_delta,
                ),
                "maximumVertexDelta": maximum_vertex_delta,
                "sourceOnlyPositionCount": len(
                    source_only_positions
                ),
                "runtimeOnlyPositionCount": len(
                    runtime_only_positions
                ),
                "unmatchedPositionCount": (
                    len(source_only_positions)
                    + len(runtime_only_positions)
                ),
            }
        )

    results.sort(
        key=bounds_delta_sort_key,
        reverse=True,
    )
    mismatches = [
        item
        for item in results
        if (
            item["maximumBoundsDelta"] > 0.0001
            or item["maximumVertexDelta"] > 0.00001
        )
    ]

    print("CAR_ALIGNMENT_VALIDATION_BEGIN")
    print(
        json.dumps(
            {
                "checkedPartCount": len(results),
                "skippedParts": skipped,
                "mismatchCount": len(mismatches),
                "maximumBoundsDelta": (
                    results[0]["maximumBoundsDelta"]
                    if results
                    else 0.0
                ),
                "wheelGroups": build_wheel_summary(
                    wheel_components
                ),
                "mismatches": mismatches,
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    print("CAR_ALIGNMENT_VALIDATION_END")


if __name__ == "__main__":
    main()
