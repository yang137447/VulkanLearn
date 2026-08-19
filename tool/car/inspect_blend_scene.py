import json
import math
import sys
from pathlib import Path

import bpy


def rounded(values):
    return [round(float(value), 9) for value in values]


def main():
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    if len(arguments) != 1:
        raise RuntimeError("Usage: blender --background file.blend --python inspect_blend_scene.py -- output.json")

    output_path = Path(arguments[0])
    mesh_objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]

    mesh_data_users = {}
    for obj in mesh_objects:
        mesh_data_users.setdefault(obj.data.name, []).append(obj.name)

    report = {
        "sourceBlend": bpy.data.filepath,
        "scene": bpy.context.scene.name,
        "objectCount": len(bpy.context.scene.objects),
        "meshObjectCount": len(mesh_objects),
        "uniqueMeshDataCount": len(mesh_data_users),
        "collections": [
            {
                "name": collection.name,
                "objectCount": len(collection.objects),
                "objects": [obj.name for obj in collection.objects],
            }
            for collection in bpy.data.collections
        ],
        "meshDataUsers": mesh_data_users,
        "objects": [],
    }

    for obj in mesh_objects:
        mesh = obj.data
        report["objects"].append(
            {
                "name": obj.name,
                "meshData": mesh.name,
                "parent": obj.parent.name if obj.parent else None,
                "collections": [collection.name for collection in obj.users_collection],
                "hideViewport": obj.hide_get(),
                "hideRender": obj.hide_render,
                "location": rounded(obj.location),
                "rotationMode": obj.rotation_mode,
                "rotationEulerDegrees": rounded(
                    math.degrees(value) for value in obj.rotation_euler
                ),
                "scale": rounded(obj.scale),
                "matrixWorld": [
                    rounded(row)
                    for row in obj.matrix_world
                ],
                "dimensions": rounded(obj.dimensions),
                "vertexCount": len(mesh.vertices),
                "polygonCount": len(mesh.polygons),
                "uvLayerCount": len(mesh.uv_layers),
                "uvLayers": [layer.name for layer in mesh.uv_layers],
                "materials": [
                    {
                        "slot": index,
                        "link": slot.link,
                        "name": slot.material.name if slot.material else None,
                    }
                    for index, slot in enumerate(obj.material_slots)
                ],
            }
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(
        "CAR_BLEND_REPORT "
        f"meshObjects={report['meshObjectCount']} "
        f"uniqueMeshData={report['uniqueMeshDataCount']} "
        f"output={output_path}"
    )


main()
