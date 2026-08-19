import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import bpy


WHEEL_PARTS = (
    ("BL", "models/SM_car_wheel_bl.json"),
    ("FR", "models/SM_car_wheel_fr.json"),
    ("FL", "models/SM_car_wheel_fl.json"),
    ("BR", "models/SM_car_wheel_br.json"),
)

TEXTURE_GROUPS = (
    ("Tread", "T_Wheel_Tread", "T_car_wheel_tread"),
    ("Hub", "T_Wheel_Hub", "T_car_wheel_hub"),
    ("Brake", "T_Wheel_Brake", "T_car_wheel_brake"),
)

TEXTURE_CHANNELS = (
    ("BaseColor", "BaseColor", "base_color", (0, 1, 2, 3)),
    ("Mask", "Mask", "mask", (0, 1, 2, 3)),
    ("Normal", "Normal", "normal", (0, 1, 2, 3)),
)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def load_json(path):
    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def quantize_uv(uv):
    return (round(float(uv[0]), 6), round(float(uv[1]), 6))


def load_obj_uvs(path):
    texture_coordinates = []
    face_uvs = []
    with path.open("r", encoding="utf-8") as input_file:
        for line in input_file:
            if line.startswith("vt "):
                fields = line.split()
                texture_coordinates.append(
                    (float(fields[1]), float(fields[2]))
                )
            elif line.startswith("f "):
                for vertex_reference in line.split()[1:]:
                    indices = vertex_reference.split("/")
                    if len(indices) < 2 or not indices[1]:
                        raise RuntimeError(
                            f"OBJ face has no UV index: {path}"
                        )
                    uv_index = int(indices[1])
                    if uv_index < 0:
                        uv_index = len(texture_coordinates) + uv_index
                    else:
                        uv_index -= 1
                    face_uvs.append(texture_coordinates[uv_index])
    return face_uvs


def uv_comparison(blender_uvs, obj_uvs):
    blender_counter = Counter(quantize_uv(uv) for uv in blender_uvs)
    obj_counter = Counter(quantize_uv(uv) for uv in obj_uvs)
    flipped_obj_counter = Counter(
        quantize_uv((uv[0], 1.0 - uv[1]))
        for uv in obj_uvs
    )
    return {
        "blenderLoopCount": len(blender_uvs),
        "objTextureCoordinateCount": len(obj_uvs),
        "directDifferenceCount": sum(
            (blender_counter - obj_counter).values()
        )
        + sum((obj_counter - blender_counter).values()),
        "vFlippedDifferenceCount": sum(
            (blender_counter - flipped_obj_counter).values()
        )
        + sum((flipped_obj_counter - blender_counter).values()),
        "blenderBounds": {
            "min": [
                round(min(uv[axis] for uv in blender_uvs), 6)
                for axis in range(2)
            ],
            "max": [
                round(max(uv[axis] for uv in blender_uvs), 6)
                for axis in range(2)
            ],
        },
        "objBounds": {
            "min": [
                round(min(uv[axis] for uv in obj_uvs), 6)
                for axis in range(2)
            ],
            "max": [
                round(max(uv[axis] for uv in obj_uvs), 6)
                for axis in range(2)
            ],
        },
    }


def image_for_name(name):
    image = bpy.data.images.get(name)
    if image is None:
        raise RuntimeError("Missing packed Blender image: " + name)
    return image


def compare_images(source_image, runtime_image, channels):
    if tuple(source_image.size) != tuple(runtime_image.size):
        return {
            "sourceSize": list(source_image.size),
            "runtimeSize": list(runtime_image.size),
            "maxDifference": None,
            "mismatchedPixels": None,
        }

    width, height = source_image.size
    source_pixels = list(source_image.pixels[:])
    runtime_pixels = list(runtime_image.pixels[:])
    source_channels = source_image.channels
    runtime_channels = runtime_image.channels
    maximum_difference = 0.0
    mismatched_pixels = 0
    channel_results = {}

    for y in range(height):
        runtime_y = height - 1 - y
        for x in range(width):
            source_offset = (y * width + x) * source_channels
            runtime_offset = (runtime_y * width + x) * runtime_channels
            pixel_mismatched = False
            for channel in channels:
                if channel >= source_channels or channel >= runtime_channels:
                    continue
                difference = abs(
                    source_pixels[source_offset + channel]
                    - runtime_pixels[runtime_offset + channel]
                )
                maximum_difference = max(maximum_difference, difference)
                channel_result = channel_results.setdefault(
                    str(channel),
                    {"maxDifference": 0.0, "mismatchedPixels": 0},
                )
                channel_result["maxDifference"] = max(
                    channel_result["maxDifference"],
                    difference,
                )
                if difference > 1.0e-5:
                    channel_result["mismatchedPixels"] += 1
                    pixel_mismatched = True
            if pixel_mismatched:
                mismatched_pixels += 1

    for result in channel_results.values():
        result["maxDifference"] = round(result["maxDifference"], 9)

    return {
        "sourceSize": list(source_image.size),
        "runtimeSize": list(runtime_image.size),
        "maxDifference": round(maximum_difference, 9),
        "mismatchedPixels": mismatched_pixels,
        "comparedChannels": list(channels),
        "perChannel": channel_results,
    }


def main():
    arguments = parse_arguments()
    resource_root = Path(arguments.resource_root)

    manifest = load_json(
        resource_root / "models/datas/car/car_asset_manifest.json"
    )
    parts_by_object = {
        part["blenderObject"]: part for part in manifest["parts"]
    }

    uv_results = []
    for object_name, fallback_model_path in WHEEL_PARTS:
        source_object = bpy.data.objects.get(object_name)
        if source_object is None:
            raise RuntimeError("Missing Blender wheel object: " + object_name)
        if source_object.type != "MESH":
            raise RuntimeError("Wheel object is not a mesh: " + object_name)
        uv_layer = source_object.data.uv_layers.active
        if uv_layer is None:
            raise RuntimeError("Wheel object has no active UV layer: " + object_name)

        part = parts_by_object.get(object_name, {})
        model_path = part.get("modelPath", fallback_model_path)
        model_json = load_json(resource_root / model_path)
        obj_path = resource_root / model_json["modelDataPath"]
        blender_uvs = [
            (loop.uv.x, loop.uv.y)
            for loop in uv_layer.data
        ]
        obj_uvs = load_obj_uvs(obj_path)
        uv_results.append(
            {
                "blenderObject": object_name,
                "assetId": part.get("assetId"),
                "modelPath": model_path,
                "objPath": str(obj_path),
                "comparison": uv_comparison(blender_uvs, obj_uvs),
            }
        )

    texture_results = []
    for group_label, source_prefix, runtime_prefix in TEXTURE_GROUPS:
        for channel_label, source_suffix, runtime_suffix, channels in TEXTURE_CHANNELS:
            source_image = image_for_name(
                f"{source_prefix}_{source_suffix}.tga"
            )
            runtime_path = (
                resource_root
                / "textures/datas/car"
                / f"{runtime_prefix}_{runtime_suffix}.tga"
            )
            runtime_image = bpy.data.images.load(
                str(runtime_path),
                check_existing=False,
            )
            # 运行时描述中的 Mask/Normal 是 linear；保持与 Blender 源图
            # 相同的解码色彩空间，比较结果才代表纹理数据本身。
            runtime_image.colorspace_settings.name = (
                source_image.colorspace_settings.name
            )
            texture_results.append(
                {
                    "group": group_label,
                    "channel": channel_label,
                    "sourceImage": source_image.name,
                    "runtimeImage": str(runtime_path),
                    "sourceColorSpace": source_image.colorspace_settings.name,
                    "runtimeColorSpace": runtime_image.colorspace_settings.name,
                    "comparison": compare_images(
                        source_image,
                        runtime_image,
                        channels,
                    ),
                }
            )

    print("CAR_WHEEL_UV_TEXTURE_COMPARISON_BEGIN")
    print(json.dumps({"uv": uv_results, "textures": texture_results}, indent=2))
    print("CAR_WHEEL_UV_TEXTURE_COMPARISON_END")


if __name__ == "__main__":
    main()
