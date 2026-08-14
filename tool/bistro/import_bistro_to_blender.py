from __future__ import annotations

import os
from pathlib import Path

import bpy


def import_unique_models(resources: Path, output_path: Path) -> None:
    data_root = resources / "models/datas/bistro_modular"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)

    imported_collections = []
    for glb in sorted(data_root.rglob("*.glb")):
        relative_stem = "_".join(glb.relative_to(data_root).with_suffix("").parts)
        collection_name = f"SM_{relative_stem}"

        bpy.ops.import_scene.gltf(filepath=str(glb))
        collection = bpy.data.collections.new(collection_name)
        bpy.context.scene.collection.children.link(collection)

        imported_objects = list(bpy.context.selected_objects)
        if not imported_objects:
            imported_objects = [obj for obj in bpy.context.scene.collection.objects]

        for obj in imported_objects:
            for old_collection in list(obj.users_collection):
                old_collection.objects.unlink(obj)
            collection.objects.link(obj)

        imported_collections.append(collection_name)

    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    print(f"Imported {len(imported_collections)} collections to {output_path}")


def main() -> None:
    resources = Path(os.environ["BISTRO_RESOURCES"])
    output = Path(os.environ["BISTRO_OUTPUT"])
    import_unique_models(resources.resolve(), output.resolve())


if __name__ == "__main__":
    main()
