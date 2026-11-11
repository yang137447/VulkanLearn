"""Convert a displacement EXR to a 16-bit PNG with Blender's image API.

Usage::

    blender.exe --background --factory-startup --python exr_to_png16.py -- \
        input.exr output.png

Blender is used because it reads EXR natively and writes 16-bit PNG without
colour-management transforms (Standard view, Non-Color input), which is what
the terrain height contract expects.
"""

import sys

import bpy


def main() -> None:
    arguments = sys.argv[sys.argv.index("--") + 1:]
    source, target = arguments[0], arguments[1]
    image = bpy.data.images.load(source)
    image.colorspace_settings.name = "Non-Color"
    scene = bpy.context.scene
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "BW"
    scene.render.image_settings.color_depth = "16"
    image.save_render(target, scene=scene)
    print("WROTE", target, image.size[0], image.size[1])


main()
