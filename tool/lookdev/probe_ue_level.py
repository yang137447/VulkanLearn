from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


LEVEL = "/Game/TemplateLevels/DataPrepView"


def vector(value) -> list[float]:
    return [float(value.x), float(value.y), float(value.z)]


def rotator(value) -> list[float]:
    return [float(value.roll), float(value.pitch), float(value.yaw)]


def main() -> None:
    unreal.EditorLevelLibrary.load_level(LEVEL)
    actors = []
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        item = {
            "name": actor.get_name(),
            "class": actor.get_class().get_name(),
            "location": vector(actor.get_actor_location()),
            "rotation": rotator(actor.get_actor_rotation()),
            "scale": vector(actor.get_actor_scale3d()),
            "components": [],
        }
        for component in actor.get_components_by_class(unreal.SceneComponent):
            component_item = {
                "name": component.get_name(),
                "class": component.get_class().get_name(),
                "relativeLocation": vector(component.get_editor_property("relative_location")),
                "relativeRotation": rotator(component.get_editor_property("relative_rotation")),
                "relativeScale": vector(component.get_editor_property("relative_scale3d")),
            }
            if isinstance(component, unreal.StaticMeshComponent):
                mesh = component.get_editor_property("static_mesh")
                component_item["staticMesh"] = mesh.get_path_name() if mesh else None
                component_item["overrideMaterials"] = [
                    material.get_path_name() if material else None
                    for material in component.get_editor_property("override_materials")
                ]
                if mesh:
                    component_item["meshMaterials"] = [
                        material.get_path_name() if material else None
                        for material in (
                            mesh.get_material(index)
                            for index in range(
                                len(mesh.get_editor_property("static_materials"))
                            )
                        )
                    ]
            item["components"].append(component_item)
        actors.append(item)
    output = Path(os.environ["VL_UE_PROBE_OUTPUT"])
    output.write_text(json.dumps(actors, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    unreal.SystemLibrary.quit_editor()


main()
