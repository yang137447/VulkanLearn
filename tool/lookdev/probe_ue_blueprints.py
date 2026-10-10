from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


ASSETS = (
    "/Game/TemplateLevels/Assets/GrayPlate",
    "/Game/TemplateLevels/Assets/AssetsPreview_BD",
)


def transform_to_dict(transform) -> dict:
    location = transform.translation
    rotation = transform.rotation.rotator()
    scale = transform.scale3d
    return {
        "location": [float(location.x), float(location.y), float(location.z)],
        "rotation": [float(rotation.roll), float(rotation.pitch), float(rotation.yaw)],
        "scale": [float(scale.x), float(scale.y), float(scale.z)],
    }


def inspect_blueprint(path: str) -> dict:
    blueprint = unreal.load_asset(path)
    record = {"path": path, "loaded": blueprint is not None}
    if blueprint is None:
        return record
    record["class"] = blueprint.get_class().get_name()
    generated_class = blueprint.generated_class()
    record["generatedClass"] = generated_class.get_name() if generated_class else None
    if generated_class is None:
        return record
    cdo = unreal.get_default_object(generated_class)
    components = []
    for component in cdo.get_components_by_class(unreal.SceneComponent):
        item = {
            "name": component.get_name(),
            "class": component.get_class().get_name(),
            "relativeTransform": transform_to_dict(component.get_relative_transform()),
        }
        if isinstance(component, unreal.StaticMeshComponent):
            mesh = component.get_editor_property("static_mesh")
            item["staticMesh"] = mesh.get_path_name() if mesh else None
            item["materials"] = [
                material.get_path_name() if material else None
                for material in component.get_editor_property("override_materials")
            ]
        components.append(item)
    record["components"] = components
    return record


def main() -> None:
    output = Path(os.environ["VL_UE_PROBE_OUTPUT"])
    output.write_text(
        json.dumps([inspect_blueprint(path) for path in ASSETS], ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    unreal.SystemLibrary.quit_editor()


main()
