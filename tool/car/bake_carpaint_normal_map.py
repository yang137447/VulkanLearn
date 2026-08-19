import argparse
import sys
from pathlib import Path

import bpy


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=2048)
    args = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(args)


def find_carpaint_slot(obj):
    for index, slot in enumerate(obj.material_slots):
        if slot.material and slot.material.name == "M_Carpaint":
            return index, slot.material
    raise RuntimeError("Object 'main' has no M_Carpaint material slot")


def main():
    arguments = parse_arguments()
    output_path = arguments.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    obj = bpy.data.objects.get("main")
    if obj is None or obj.type != "MESH":
        raise RuntimeError("Car_02.blend does not contain mesh object 'main'")
    if not obj.data.uv_layers:
        raise RuntimeError("Object 'main' has no UV layer")

    material_index, material = find_carpaint_slot(obj)
    image = bpy.data.images.new(
        "T_car_carpaint_normal_baked",
        width=arguments.size,
        height=arguments.size,
        alpha=True,
        float_buffer=False,
    )
    image.generated_color = (0.5, 0.5, 1.0, 1.0)

    image_node = material.node_tree.nodes.new("ShaderNodeTexImage")
    image_node.name = "BakeTarget_CarpaintNormal"
    image_node.label = "Bake target"
    image_node.image = image
    material.node_tree.nodes.active = image_node
    image_node.select = True
    for node in material.node_tree.nodes:
        if node != image_node:
            node.select = False

    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj.active_material_index = material_index

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.image_settings.file_format = "TARGA"
    scene.render.image_settings.color_mode = "RGBA"

    # Blender's bake operator requires the Cycles backend even though the source
    # material itself is evaluated from the same node graph.
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 16
    scene.cycles.use_denoising = False
    bpy.ops.object.bake(
        type="NORMAL",
        normal_space="TANGENT",
        margin=16,
        use_clear=True,
    )

    image.filepath_raw = str(output_path)
    image.file_format = "TARGA"
    image.save()
    print(
        "CARPAINT_NORMAL_BAKED "
        f"size={arguments.size} output={output_path}"
    )


if __name__ == "__main__":
    main()
