import sys
from pathlib import Path

import addon_utils
import bpy


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 2:
        raise SystemExit(
            "usage: blender --background --python import_3d66_betterfbx.py -- "
            "<fbx> <blend>"
        )
    return Path(argv[0]), Path(argv[1])


def main():
    fbx_path, blend_path = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("better_fbx", default_set=True)
    result = bpy.ops.better_import.fbx(
        filepath=str(fbx_path),
        files=[{"name": fbx_path.name}],
        use_animation=False,
        use_vertex_animation=False,
        use_import_materials=True,
        my_import_normal="Import",
        my_fbx_unit="cm",
        my_scale=1.0,
        use_optimize_for_blender=False,
        use_fix_mesh_scaling=False,
        use_separate_collection=False,
    )
    if "FINISHED" not in result:
        raise SystemExit(f"BetterFBX import failed: {result}")
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
    print(
        f"objects={len(bpy.context.scene.objects)} "
        f"meshes={sum(1 for obj in bpy.context.scene.objects if obj.type == 'MESH')} "
        f"materials={len(bpy.data.materials)} images={len(bpy.data.images)}"
    )
    print(f"saved {blend_path}")


if __name__ == "__main__":
    main()
