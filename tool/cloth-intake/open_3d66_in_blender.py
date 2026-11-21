import sys
from pathlib import Path

import bpy


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 2:
        raise SystemExit(
            "usage: blender --background --python open_3d66_in_blender.py -- "
            "<fbx> <blend>"
        )
    return Path(argv[0]), Path(argv[1])


def main():
    fbx_path, blend_path = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(
        filepath=str(fbx_path),
        use_custom_normals=True,
        use_image_search=True,
    )
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    scene.unit_settings.length_unit = "CENTIMETERS"
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == "VIEW_3D":
                for space in area.spaces:
                    if space.type == "VIEW_3D":
                        space.clip_start = 0.001
                        space.clip_end = 10000.0
    bpy.ops.object.select_all(action="DESELECT")
    blend_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    print(f"saved {blend_path}")


if __name__ == "__main__":
    main()
