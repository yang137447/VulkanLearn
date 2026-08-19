import argparse
import json
import math
import sys
from collections import Counter
from pathlib import Path

import bpy


WHEEL_OBJECT_NAMES = (
    "BL",
    "FR",
    "FL",
    "BR",
)
BRAKE_MATERIAL_NAME = "M_Wheel_Brake"


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    parser.add_argument(
        "--manifest",
        default="models/datas/car/car_asset_manifest.json",
    )
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def load_json(path):
    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def load_obj_uvs(path, material_name):
    texture_coordinates = []
    face_uvs = []
    active_material = None
    with path.open("r", encoding="utf-8") as input_file:
        for line in input_file:
            if line.startswith("vt "):
                fields = line.split()
                texture_coordinates.append(
                    (float(fields[1]), float(fields[2]))
                )
            elif line.startswith("usemtl "):
                active_material = line.split(maxsplit=1)[1].strip()
            elif (
                line.startswith("f ")
                and active_material == material_name
            ):
                for vertex_reference in line.split()[1:]:
                    indices = vertex_reference.split("/")
                    if len(indices) < 2 or not indices[1]:
                        raise RuntimeError(
                            f"OBJ face has no UV index: {path}"
                        )
                    uv_index = int(indices[1])
                    if uv_index < 0:
                        uv_index = (
                            len(texture_coordinates) + uv_index
                        )
                    else:
                        uv_index -= 1
                    face_uvs.append(
                        texture_coordinates[uv_index]
                    )
    return face_uvs


def quantize_uv(uv):
    return (round(float(uv[0]), 5), round(float(uv[1]), 5))


def compare_uv_sets(blender_uvs, obj_uvs):
    blender_counter = Counter(quantize_uv(uv) for uv in blender_uvs)
    obj_counter = Counter(quantize_uv(uv) for uv in obj_uvs)
    flipped_obj_counter = Counter(
        quantize_uv((uv[0], 1.0 - uv[1]))
        for uv in obj_uvs
    )
    return {
        "directDifferenceCount": sum(
            (blender_counter - obj_counter).values()
        )
        + sum((obj_counter - blender_counter).values()),
        "flippedDifferenceCount": sum(
            (blender_counter - flipped_obj_counter).values()
        )
        + sum((flipped_obj_counter - blender_counter).values()),
    }


def find_base_color_image(material):
    if material is None or not material.use_nodes:
        return None
    principled_nodes = [
        node
        for node in material.node_tree.nodes
        if node.type == "BSDF_PRINCIPLED"
    ]
    if not principled_nodes:
        return None

    base_color_input = principled_nodes[0].inputs.get("Base Color")
    if base_color_input is None or not base_color_input.is_linked:
        return None
    source_node = base_color_input.links[0].from_node
    if source_node.type != "TEX_IMAGE":
        return None
    return source_node.image


def wrap_coordinate(value):
    return value - math.floor(value)


def sample_image(image, uv):
    width, height = image.size
    u = wrap_coordinate(float(uv[0]))
    v = wrap_coordinate(float(uv[1]))
    x = min(int(u * width), width - 1)
    # bpy.image.pixels 的行序与纹理节点的 UV 纵向约定相反；
    # 这里翻一次 V，得到与 Blender 材质节点一致的采样结果。
    y = min(int((1.0 - v) * height), height - 1)
    offset = (y * width + x) * image.channels
    pixels = image.pixels
    return tuple(float(pixels[offset + channel]) for channel in range(3))


def summarize_samples(samples):
    if not samples:
        return {}
    count = len(samples)
    mean = [
        sum(sample[channel] for sample in samples) / count
        for channel in range(3)
    ]
    red_count = sum(
        1
        for sample in samples
        if sample[0] > sample[1] * 2.0
        and sample[0] > sample[2] * 2.0
        and sample[0] > 0.1
    )
    dark_count = sum(
        1
        for sample in samples
        if max(sample) < 0.08
    )
    return {
        "meanLinearRgb": [round(value, 6) for value in mean],
        "redSampleRatio": round(red_count / count, 6),
        "darkSampleRatio": round(dark_count / count, 6),
    }


def collect_material_uvs(
    mesh,
    uv_layer,
    material_index,
):
    loop_uvs = []
    center_uvs = []
    for polygon in mesh.polygons:
        if polygon.material_index != material_index:
            continue

        uv_sum = [0.0, 0.0]
        for loop_index in polygon.loop_indices:
            uv = uv_layer.data[loop_index].uv
            loop_uvs.append((uv.x, uv.y))
            uv_sum[0] += uv.x
            uv_sum[1] += uv.y
        inverse_count = 1.0 / polygon.loop_total
        center_uvs.append(
            (
                uv_sum[0] * inverse_count,
                uv_sum[1] * inverse_count,
            )
        )
    return loop_uvs, center_uvs


def main():
    arguments = parse_arguments()
    resource_root = Path(arguments.resource_root)
    manifest = load_json(resource_root / arguments.manifest)
    parts_by_object = {
        part["blenderObject"]: part
        for part in manifest["parts"]
    }

    results = []
    for object_name in WHEEL_OBJECT_NAMES:
        source_object = bpy.data.objects.get(object_name)
        if source_object is None:
            raise RuntimeError(
                "Missing Blender object: " + object_name
            )
        if source_object.type != "MESH":
            raise RuntimeError(
                "Blender object is not a mesh: " + object_name
            )

        mesh = source_object.data
        uv_layer = mesh.uv_layers.active
        if uv_layer is None:
            raise RuntimeError(
                "Blender object has no active UV layer: "
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

        material_slot_index = next(
            (
                index
                for index, slot in enumerate(
                    source_object.material_slots
                )
                if (
                    slot.material is not None
                    and slot.material.name
                    == BRAKE_MATERIAL_NAME
                )
            ),
            None,
        )
        if material_slot_index is None:
            raise RuntimeError(
                f"Wheel '{object_name}' has no "
                f"{BRAKE_MATERIAL_NAME} slot"
            )

        blender_loop_uvs, polygon_center_uvs = (
            collect_material_uvs(
                mesh,
                uv_layer,
                material_slot_index,
            )
        )
        obj_uvs = load_obj_uvs(
            obj_path,
            BRAKE_MATERIAL_NAME,
        )
        if not blender_loop_uvs or not obj_uvs:
            raise RuntimeError(
                f"Wheel '{object_name}' has no brake UV data"
            )

        material = source_object.material_slots[
            material_slot_index
        ].material
        base_color_image = find_base_color_image(material)
        if base_color_image is None:
            raise RuntimeError(
                "Cannot resolve Base Color image for "
                + object_name
            )

        direct_samples = [
            sample_image(base_color_image, uv)
            for uv in polygon_center_uvs
        ]
        flipped_samples = [
            sample_image(
                base_color_image,
                (uv[0], 1.0 - uv[1]),
            )
            for uv in polygon_center_uvs
        ]

        results.append(
            {
                "blenderObject": object_name,
                "parent": part.get("parent"),
                "assetId": part["assetId"],
                "objPath": str(obj_path),
                "material": BRAKE_MATERIAL_NAME,
                "polygonCount": len(
                    polygon_center_uvs
                ),
                "loopCount": len(blender_loop_uvs),
                "uvBounds": {
                    "minimum": [
                        round(
                            min(
                                uv[axis]
                                for uv in blender_loop_uvs
                            ),
                            6,
                        )
                        for axis in range(2)
                    ],
                    "maximum": [
                        round(
                            max(
                                uv[axis]
                                for uv in blender_loop_uvs
                            ),
                            6,
                        )
                        for axis in range(2)
                    ],
                },
                "uvComparison": compare_uv_sets(
                    blender_loop_uvs,
                    obj_uvs,
                ),
                "baseColorImage": {
                    "name": base_color_image.name,
                    "path": bpy.path.abspath(
                        base_color_image.filepath
                    ),
                    "size": list(base_color_image.size),
                    "colorSpace": (
                        base_color_image.colorspace_settings.name
                    ),
                },
                "blenderUvSamples": summarize_samples(
                    direct_samples
                ),
                "flippedUvSamples": summarize_samples(
                    flipped_samples
                ),
            }
        )

    print("CAR_BRAKE_MATERIAL_INSPECTION_BEGIN")
    print(json.dumps(results, indent=2))
    print("CAR_BRAKE_MATERIAL_INSPECTION_END")


if __name__ == "__main__":
    main()
