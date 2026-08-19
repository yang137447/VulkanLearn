import argparse
import sys
from pathlib import Path

import bpy


TEXTURE_GROUPS = (
    (
        "Brake",
        "Wheel_Brake",
        "wheel_brake",
    ),
    (
        "Hub",
        "Wheel_Hub",
        "wheel_hub",
    ),
    (
        "Tread",
        "Wheel_Tread",
        "wheel_tread",
    ),
)

TEXTURE_CHANNELS = (
    ("BaseColor", "BaseColor", "base_color", (0, 1, 2, 3)),
    ("Mask", "Mask", "mask", (0, 1, 3)),
    ("Normal", "Normal", "normal", (0, 1, 3)),
)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--runtime-root", required=True)
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def compare_vertical_flip(source_image, runtime_image, channels):
    width, height = source_image.size
    if list(runtime_image.size) != [width, height]:
        raise RuntimeError("Texture dimensions do not match")
    if source_image.channels != runtime_image.channels:
        raise RuntimeError("Texture channel counts do not match")

    source_pixels = source_image.pixels[:]
    runtime_pixels = runtime_image.pixels[:]
    channel_count = source_image.channels
    maximum_difference = 0.0
    mismatched_pixel_count = 0

    for y in range(height):
        runtime_y = height - 1 - y
        for x in range(width):
            source_offset = (
                (y * width + x) * channel_count
            )
            runtime_offset = (
                (runtime_y * width + x) * channel_count
            )
            pixel_mismatched = False
            for channel in channels:
                difference = abs(
                    source_pixels[source_offset + channel]
                    - runtime_pixels[
                        runtime_offset + channel
                    ]
                )
                maximum_difference = max(
                    maximum_difference,
                    difference,
                )
                if difference > 1.0e-7:
                    pixel_mismatched = True
            if pixel_mismatched:
                mismatched_pixel_count += 1

    return maximum_difference, mismatched_pixel_count


def main():
    arguments = parse_arguments()
    source_root = Path(arguments.source_root)
    runtime_root = Path(arguments.runtime_root)

    for group_label, source_prefix, runtime_prefix in (
        TEXTURE_GROUPS
    ):
        for (
            channel_label,
            source_suffix,
            runtime_suffix,
            channels,
        ) in TEXTURE_CHANNELS:
            source_image = bpy.data.images.load(
                str(
                    source_root
                    / f"T_{source_prefix}_{source_suffix}.tga"
                ),
                check_existing=False,
            )
            runtime_image = bpy.data.images.load(
                str(
                    runtime_root
                    / f"T_car_{runtime_prefix}_{runtime_suffix}.tga"
                ),
                check_existing=False,
            )
            maximum_difference, mismatch_count = (
                compare_vertical_flip(
                    source_image,
                    runtime_image,
                    channels,
                )
            )
            print(
                f"{group_label}.{channel_label}: "
                f"comparedChannels={channels} "
                f"maxDifference={maximum_difference:.9f} "
                f"mismatchedPixels={mismatch_count}"
            )


if __name__ == "__main__":
    main()
