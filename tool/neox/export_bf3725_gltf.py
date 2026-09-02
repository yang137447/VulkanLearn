from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import bpy


SOURCE_OBJECT_KEY = "vulkanlearn_source_object"
SOURCE_UV_COUNT_KEY = "vulkanlearn_source_uv_count"
SOURCE_UV_NAMES_KEY = "vulkanlearn_source_uv_names"
SOURCE_COLOR_KEY = "vulkanlearn_source_has_color0"
SOURCE_MATERIAL_NAMES_KEY = "vulkanlearn_source_material_names"


def parse_arguments() -> argparse.Namespace:
    script_arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(
        description="Bake and export the current b_f_3725 assembly as auditable static glTF."
    )
    parser.add_argument(
        "--output",
        default=r"D:\YYBWorkSpace\GitHub\VukanLearnResources\Maps\SC_b_f_3725\Source\Models\b_f_3725\b_f_3725.gltf",
        help="Destination .gltf file. The .bin and textures are written beside it.",
    )
    parser.add_argument(
        "--objects",
        nargs="*",
        default=None,
        help="Optional mesh object names. By default all visible mesh objects are exported.",
    )
    parser.add_argument(
        "--include-hidden",
        action="store_true",
        help="Include hidden mesh objects when --objects is not specified.",
    )
    return parser.parse_args(script_arguments)


def collect_mesh_objects(arguments: argparse.Namespace) -> list[bpy.types.Object]:
    if arguments.objects:
        missing_names = [name for name in arguments.objects if bpy.data.objects.get(name) is None]
        if missing_names:
            raise RuntimeError("Missing Blender objects: %s" % ", ".join(missing_names))
        mesh_objects = [bpy.data.objects[name] for name in arguments.objects]
        invalid_names = [obj.name for obj in mesh_objects if obj.type != "MESH"]
        if invalid_names:
            raise RuntimeError("Requested objects are not meshes: %s" % ", ".join(invalid_names))
        return mesh_objects

    mesh_objects = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        if not arguments.include_hidden and (obj.hide_get() or obj.hide_viewport):
            continue
        mesh_objects.append(obj)
    if not mesh_objects:
        raise RuntimeError("No mesh objects selected for export")
    return mesh_objects


def get_active_color_name(mesh: bpy.types.Mesh) -> str | None:
    active_color = mesh.color_attributes.active_color
    return active_color.name if active_color is not None else None


def get_material_names(obj: bpy.types.Object) -> list[str | None]:
    return [
        slot.material.name if slot.material is not None else None
        for slot in obj.material_slots
    ]


def build_source_audit(mesh_objects: list[bpy.types.Object]) -> list[dict[str, Any]]:
    # 在应用 Modifier 和世界变换前冻结源合同，后续用它验证 UV 层、顶点色和材质槽没有被烘焙过程篡改。
    source_audit = []
    for obj in sorted(mesh_objects, key=lambda value: value.name):
        uv_layer_names = [layer.name for layer in obj.data.uv_layers]
        source_audit.append(
            {
                "object": obj.name,
                "mesh": obj.data.name,
                "vertexCount": len(obj.data.vertices),
                "polygonCount": len(obj.data.polygons),
                "uvLayerCount": len(uv_layer_names),
                "uvLayerNames": uv_layer_names,
                "exportsTexCoord1": len(uv_layer_names) >= 2,
                "colorAttributeNames": [attribute.name for attribute in obj.data.color_attributes],
                "activeColorAttribute": get_active_color_name(obj.data),
                "materialSlots": get_material_names(obj),
            }
        )
    return source_audit


def create_static_export_objects(
    mesh_objects: list[bpy.types.Object],
) -> tuple[bpy.types.Collection, list[bpy.types.Object], list[dict[str, Any]]]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    collection = bpy.data.collections.new("VulkanLearn_b_f_3725_StaticExport")
    bpy.context.scene.collection.children.link(collection)
    export_objects = []
    baked_audit = []

    for source_object in sorted(mesh_objects, key=lambda value: value.name):
        evaluated_object = source_object.evaluated_get(depsgraph)
        # preserve_all_data_layers 是真实 UV1/顶点色能进入 glTF 的前提；缺失 UV1 时绝不从 UV0 合成。
        baked_mesh = bpy.data.meshes.new_from_object(
            evaluated_object,
            preserve_all_data_layers=True,
            depsgraph=depsgraph,
        )
        if baked_mesh is None:
            raise RuntimeError("Failed to evaluate mesh: %s" % source_object.name)

        # VulkanLearn 当前读取静态顶点流，因此把当前 Pose、Modifier 和对象世界变换一次性烘进顶点。
        baked_mesh.name = source_object.data.name
        baked_mesh.transform(evaluated_object.matrix_world)
        # 镜像世界变换会改变绕序；翻转法线用于保持烘焙前可见面的朝向语义。
        if evaluated_object.matrix_world.to_3x3().determinant() < 0.0:
            baked_mesh.flip_normals()

        source_material_names = get_material_names(source_object)
        if any(name is None for name in source_material_names):
            raise RuntimeError("Mesh has an empty material slot: %s" % source_object.name)
        # 重新挂回源材质槽前先保存 polygon 索引，保证 glTF primitive 顺序仍能按 NeoX 槽名审计和绑定。
        material_indices = [polygon.material_index for polygon in baked_mesh.polygons]
        baked_mesh.materials.clear()
        for slot in source_object.material_slots:
            baked_mesh.materials.append(slot.material)
        for polygon, material_index in zip(baked_mesh.polygons, material_indices):
            polygon.material_index = material_index
        baked_mesh.update()

        source_uv_names = [layer.name for layer in source_object.data.uv_layers]
        baked_uv_names = [layer.name for layer in baked_mesh.uv_layers]
        if baked_uv_names != source_uv_names:
            raise RuntimeError(
                "%s UV layers changed during static bake: source=%s baked=%s"
                % (source_object.name, source_uv_names, baked_uv_names)
            )

        baked_object = bpy.data.objects.new(source_object.name, baked_mesh)
        collection.objects.link(baked_object)
        # extras 只承载导出审计元数据，不进入 VulkanLearn 运行时材质或场景合同。
        baked_object[SOURCE_OBJECT_KEY] = source_object.name
        baked_object[SOURCE_UV_COUNT_KEY] = len(source_uv_names)
        baked_object[SOURCE_UV_NAMES_KEY] = "|".join(source_uv_names)
        baked_object[SOURCE_COLOR_KEY] = get_active_color_name(baked_mesh) is not None
        baked_object[SOURCE_MATERIAL_NAMES_KEY] = json.dumps(
            source_material_names,
            ensure_ascii=False,
        )
        export_objects.append(baked_object)

        baked_audit.append(
            {
                "object": source_object.name,
                "mesh": baked_mesh.name,
                "vertexCount": len(baked_mesh.vertices),
                "polygonCount": len(baked_mesh.polygons),
                "uvLayerNames": baked_uv_names,
                "activeColorAttribute": get_active_color_name(baked_mesh),
                "materialSlots": source_material_names,
                "worldTransformBaked": True,
                "modifiersApplied": True,
            }
        )

    return collection, export_objects, baked_audit


def remove_static_export_objects(
    collection: bpy.types.Collection,
    export_objects: list[bpy.types.Object],
) -> None:
    # 临时集合只服务本次导出；无论导出成功与否都清理，不能污染用户正在编辑的 NeoxIO 场景。
    baked_meshes = [obj.data for obj in export_objects]
    for obj in export_objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    for mesh in baked_meshes:
        bpy.data.meshes.remove(mesh)
    bpy.data.collections.remove(collection)


def export_gltf(output_path: Path, export_objects: list[bpy.types.Object]) -> None:
    if output_path.suffix.lower() != ".gltf":
        raise RuntimeError("Output must use .gltf so TEXCOORD semantics can be audited")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if bpy.context.object is not None and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    for obj in export_objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = export_objects[0]

    # VIEWPORT 材质只保留槽位/primitive 身份，避免 Blender 再复制一套 NeoX 纹理；正式纹理由 MI_ 资产绑定。
    result = bpy.ops.export_scene.gltf(
        filepath=str(output_path),
        check_existing=False,
        export_format="GLTF_SEPARATE",
        use_selection=True,
        export_texcoords=True,
        export_normals=True,
        export_tangents=True,
        export_vertex_color="ACTIVE",
        export_all_vertex_colors=False,
        export_active_vertex_color_when_no_material=True,
        export_materials="VIEWPORT",
        export_extras=True,
        export_skins=False,
        export_animations=False,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender glTF export did not finish: %s" % sorted(result))


def is_identity_node_transform(node: dict[str, Any]) -> bool:
    return (
        node.get("matrix") is None
        and node.get("translation", [0.0, 0.0, 0.0]) == [0.0, 0.0, 0.0]
        and node.get("rotation", [0.0, 0.0, 0.0, 1.0]) == [0.0, 0.0, 0.0, 1.0]
        and node.get("scale", [1.0, 1.0, 1.0]) == [1.0, 1.0, 1.0]
    )


def validate_export(output_path: Path, expected_object_names: set[str]) -> list[dict[str, Any]]:
    with output_path.open("r", encoding="utf-8") as stream:
        gltf = json.load(stream)

    if gltf.get("skins"):
        raise RuntimeError("Static VulkanLearn export unexpectedly contains glTF skins")

    meshes = gltf.get("meshes", [])
    materials = gltf.get("materials", [])
    validation = []
    validated_object_names = set()
    for node in gltf.get("nodes", []):
        extras = node.get("extras", {})
        source_object = extras.get(SOURCE_OBJECT_KEY)
        if source_object not in expected_object_names or "mesh" not in node:
            continue
        if not is_identity_node_transform(node):
            raise RuntimeError("Static mesh node still has a transform: %s" % source_object)

        source_uv_count = int(extras[SOURCE_UV_COUNT_KEY])
        source_has_color0 = bool(extras[SOURCE_COLOR_KEY])
        source_material_names = json.loads(extras[SOURCE_MATERIAL_NAMES_KEY])
        gltf_mesh = meshes[node["mesh"]]
        primitive_audit = []
        exported_material_names = []
        for primitive_index, primitive in enumerate(gltf_mesh.get("primitives", [])):
            attributes = primitive.get("attributes", {})
            texcoord_semantics = sorted(
                semantic for semantic in attributes if semantic.startswith("TEXCOORD_")
            )
            # 2U 合同必须双向精确：源有第二 UV 才导 TEXCOORD_1，源没有时导出结果也不得出现合成 UV1。
            has_texcoord1 = "TEXCOORD_1" in attributes
            expected_texcoord1 = source_uv_count >= 2
            if has_texcoord1 != expected_texcoord1:
                raise RuntimeError(
                    "%s primitive %d TEXCOORD_1 mismatch: source UV layers=%d, attributes=%s"
                    % (source_object, primitive_index, source_uv_count, texcoord_semantics)
                )
            has_color0 = "COLOR_0" in attributes
            if has_color0 != source_has_color0:
                raise RuntimeError(
                    "%s primitive %d COLOR_0 mismatch: source active color=%s, attributes=%s"
                    % (source_object, primitive_index, source_has_color0, sorted(attributes))
                )

            material_index = primitive.get("material")
            material_name = materials[material_index].get("name") if material_index is not None else None
            exported_material_names.append(material_name)
            primitive_audit.append(
                {
                    "primitive": primitive_index,
                    "material": material_index,
                    "materialName": material_name,
                    "texCoordSemantics": texcoord_semantics,
                    "hasColor0": has_color0,
                }
            )

        # 材质槽顺序决定模型 JSON 的名称绑定；顺序变化必须在离线阶段失败，不能留到运行时错绑材质。
        if exported_material_names != source_material_names:
            raise RuntimeError(
                "%s material primitive order mismatch: source=%s exported=%s"
                % (source_object, source_material_names, exported_material_names)
            )

        validation.append(
            {
                "object": source_object,
                "gltfMesh": gltf_mesh.get("name"),
                "sourceUvLayerCount": source_uv_count,
                "primitives": primitive_audit,
            }
        )
        validated_object_names.add(source_object)

    missing_objects = sorted(expected_object_names - validated_object_names)
    if missing_objects:
        raise RuntimeError("Exported glTF is missing audited mesh nodes: %s" % ", ".join(missing_objects))
    return validation


def write_audit(
    output_path: Path,
    source_audit: list[dict[str, Any]],
    baked_audit: list[dict[str, Any]],
    gltf_audit: list[dict[str, Any]],
) -> Path:
    audit_path = output_path.with_suffix(".audit.json")
    audit = {
        "sourceBlend": bpy.data.filepath,
        "outputGltf": str(output_path),
        "staticBakePolicy": {
            "modifiersApplied": True,
            "worldTransformsBaked": True,
            "skinsExported": False,
        },
        "uvPolicy": {
            "uv0Only": "export TEXCOORD_0 only",
            "uv0AndUv1": "export TEXCOORD_0 and TEXCOORD_1",
            "syntheticUv1": False,
        },
        "sourceMeshes": source_audit,
        "bakedMeshes": baked_audit,
        "gltfValidation": gltf_audit,
    }
    with audit_path.open("w", encoding="utf-8") as stream:
        json.dump(audit, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return audit_path


def main() -> None:
    arguments = parse_arguments()
    output_path = Path(arguments.output).expanduser().resolve()
    mesh_objects = collect_mesh_objects(arguments)
    source_audit = build_source_audit(mesh_objects)

    # VulkanLearn 当前按静态顶点流读取模型，不消费骨骼和节点变换，因此在导出边界烘焙当前姿态。
    collection, export_objects, baked_audit = create_static_export_objects(mesh_objects)
    try:
        export_gltf(output_path, export_objects)
    finally:
        remove_static_export_objects(collection, export_objects)

    gltf_audit = validate_export(output_path, {obj.name for obj in mesh_objects})
    audit_path = write_audit(output_path, source_audit, baked_audit, gltf_audit)
    print("Exported static glTF: %s" % output_path)
    print("UV/material audit: %s" % audit_path)


if __name__ == "__main__":
    main()
