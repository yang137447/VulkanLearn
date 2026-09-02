import argparse
import json
import sys
from pathlib import Path

import bpy


TEXTURES = (
    ("grille_base_color", "T_Grid_BaseColor.tga", "T_car_grille_base_color.tga", (0, 1, 2, 3)),
    ("grille_mask", "T_Grid_Mask.tga", "T_car_grille_mask.tga", (0, 1, 3)),
    ("grille_normal", "T_Grid_Nomral.tga", "T_car_grille_normal.tga", (0, 1, 3)),
    ("inner_base_color", "T_Inner_BaseColor.tga", "T_car_inner_base_color.tga", (0, 1, 2, 3)),
    ("inner_mask", "T_Inner_Mask.tga", "T_car_inner_mask.tga", (0, 1, 3)),
    ("inner_normal", "T_Inner_Normal.tga", "T_car_inner_normal.tga", (0, 1, 3)),
    (
        "license_plate_base_color",
        "T_LicensePlate_BaseColor.tga",
        "T_car_license_plate_base_color.tga",
        (0, 1, 2, 3),
    ),
    ("license_plate_mask", "T_LicensePlate_Mask.tga", "T_car_license_plate_mask.tga", (0, 1, 3)),
    ("light_base_color", "T_Light_BaseColor.tga", "T_car_light_base_color.tga", (0, 1, 2, 3)),
    ("light_mask", "T_Light_Mask.tga", "T_car_light_mask.tga", (0, 1, 3)),
    ("light_normal", "T_Light_Normal.tga", "T_car_light_normal.tga", (0, 1, 3)),
    ("light_emissive", "T_Light_Emissive.tga", "T_car_light_emissive.tga", (0, 1, 2, 3)),
    ("logo_base_color", "T_Logo_BaseColor.tga", "T_car_logo_base_color.tga", (0, 1, 2, 3)),
    ("logo_mask", "T_Logo_Mask.tga", "T_car_logo_mask.tga", (0, 1, 3)),
    ("logo_normal", "T_Logo_Normal.tga", "T_car_logo_normal.tga", (0, 1, 3)),
    ("exterior_trim_base_color", "T_Out_BaseColor.tga", "T_car_exterior_trim_base_color.tga", (0, 1, 2, 3)),
    ("exterior_trim_mask", "T_Out_Mask.tga", "T_car_exterior_trim_mask.tga", (0, 1, 3)),
    (
        "wheel_tread_base_color",
        "T_Wheel_Tread_BaseColor.tga",
        "T_car_wheel_tread_base_color.tga",
        (0, 1, 2, 3),
    ),
    ("wheel_tread_mask", "T_Wheel_Tread_Mask.tga", "T_car_wheel_tread_mask.tga", (0, 1, 3)),
    ("wheel_tread_normal", "T_Wheel_Tread_Normal.tga", "T_car_wheel_tread_normal.tga", (0, 1, 3)),
    (
        "wheel_hub_base_color",
        "T_Wheel_Hub_BaseColor.tga",
        "T_car_wheel_hub_base_color.tga",
        (0, 1, 2, 3),
    ),
    ("wheel_hub_mask", "T_Wheel_Hub_Mask.tga", "T_car_wheel_hub_mask.tga", (0, 1, 3)),
    ("wheel_hub_normal", "T_Wheel_Hub_Normal.tga", "T_car_wheel_hub_normal.tga", (0, 1, 3)),
    (
        "wheel_brake_base_color",
        "T_Wheel_Brake_BaseColor.tga",
        "T_car_wheel_brake_base_color.tga",
        (0, 1, 2, 3),
    ),
    ("wheel_brake_mask", "T_Wheel_Brake_Mask.tga", "T_car_wheel_brake_mask.tga", (0, 1, 3)),
    ("wheel_brake_normal", "T_Wheel_Brake_Normal.tga", "T_car_wheel_brake_normal.tga", (0, 1, 3)),
)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def compare(source_image, runtime_image, channels, flip_runtime_y):
    if tuple(source_image.size) != tuple(runtime_image.size):
        return {
            "mismatchedPixels": None,
            "maximumDifference": None,
        }

    width, height = source_image.size
    source_pixels = list(source_image.pixels[:])
    runtime_pixels = list(runtime_image.pixels[:])
    source_channels = source_image.channels
    runtime_channels = runtime_image.channels
    mismatched_pixels = 0
    maximum_difference = 0.0

    for y in range(height):
        runtime_y = height - 1 - y if flip_runtime_y else y
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
                if difference > 1.0e-5:
                    pixel_mismatched = True
            if pixel_mismatched:
                mismatched_pixels += 1

    return {
        "mismatchedPixels": mismatched_pixels,
        "maximumDifference": round(maximum_difference, 9),
    }


def main():
    arguments = parse_arguments()
    runtime_root = Path(arguments.resource_root) / "Maps" / "SC_car_showcase" / "Source" / "Textures" / "car"
    results = []

    for label, source_name, runtime_name, channels in TEXTURES:
        source_image = bpy.data.images.get(source_name)
        if source_image is None:
            raise RuntimeError(f"Missing Blender image: {source_name}")

        runtime_path = runtime_root / runtime_name
        runtime_image = bpy.data.images.load(
            str(runtime_path),
            check_existing=False,
        )
        runtime_image.colorspace_settings.name = (
            source_image.colorspace_settings.name
        )

        direct = compare(
            source_image,
            runtime_image,
            channels,
            False,
        )
        flipped = compare(
            source_image,
            runtime_image,
            channels,
            True,
        )
        orientation = (
            "direct"
            if direct["mismatchedPixels"] <= flipped["mismatchedPixels"]
            else "verticalFlip"
        )
        results.append(
            {
                "label": label,
                "source": source_name,
                "runtime": str(runtime_path),
                "channels": list(channels),
                "direct": direct,
                "verticalFlip": flipped,
                "bestMatch": orientation,
            }
        )

    print("CAR_TEXTURE_ORIENTATION_BEGIN")
    print(json.dumps(results, ensure_ascii=False, indent=2))
    print("CAR_TEXTURE_ORIENTATION_END")


main()
