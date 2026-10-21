"""Export DefaultLevel's authored static geometry from an isolated UE copy.

Uses UE's LOD0 CPU section reader, not guessed boxes or screenshot reconstruction.
No asset/project saves, simulation, material baking or original-project writes.
"""
import json
import os
from pathlib import Path
import traceback
import unreal


def vector(value):
    return [float(value.x), float(value.y), float(value.z)]


def transform(value):
    q = value.rotation
    return {"position": vector(value.translation), "scale": vector(value.scale3d),
            "quaternion": [float(q.x), float(q.y), float(q.z), float(q.w)]}


def main():
    output = Path(os.environ["VL_GASP_LEVEL_OUTPUT"]).resolve()
    project = Path(unreal.Paths.project_dir()).resolve()
    if output.is_relative_to(project):
        raise RuntimeError("Export output must be outside the isolated UE project")
    if output.exists():
        raise RuntimeError("Choose a new export directory")
    output.mkdir(parents=True)
    report = {"level": "/Game/Levels/DefaultLevel",
              "engine": unreal.SystemLibrary.get_engine_version(),
              "project": str(project),
              "meshes": {}, "instances": [], "starts": [], "skipped": [], "errors": []}
    try:
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not levels.load_level(report["level"]):
            raise RuntimeError("Could not load DefaultLevel")
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
        for actor in actors:
            if isinstance(actor, unreal.PlayerStart):
                report["starts"].append({"name": actor.get_name(),
                                         "transform": transform(actor.get_actor_transform())})
            components = actor.get_components_by_class(unreal.StaticMeshComponent)
            if not components:
                report["skipped"].append({"actor": actor.get_name(), "class": actor.get_class().get_name()})
            for component in components:
                mesh = component.get_editor_property("static_mesh")
                if mesh is None or not component.is_visible() or component.get_editor_property("hidden_in_game"):
                    continue
                key = mesh.get_path_name()
                if key not in report["meshes"]:
                    sections = []
                    for section in range(mesh.get_num_sections(0)):
                        vertices, indices, normals, uvs, _ = unreal.ProceduralMeshLibrary.get_section_from_static_mesh(
                            mesh, 0, section)
                        if not vertices or not indices:
                            raise RuntimeError("Empty LOD0 CPU section: " + key)
                        material = mesh.get_material(section)
                        sections.append({"vertices": [vector(v) for v in vertices], "indices": list(indices),
                                         "normals": [vector(n) for n in normals],
                                         "uvs": [[float(uv.x), float(uv.y)] for uv in uvs],
                                         "material": material.get_path_name() if material else ""})
                    filename = f"mesh_{len(report['meshes']):03d}.json"
                    (output / filename).write_text(json.dumps(sections), encoding="utf-8")
                    report["meshes"][key] = {"file": filename, "sections": len(sections)}
                transforms = ([component.get_instance_transform(index, world_space=True)
                               for index in range(component.get_instance_count())]
                              if isinstance(component, unreal.InstancedStaticMeshComponent)
                              else [component.get_world_transform()])
                materials = [component.get_material(i).get_path_name() if component.get_material(i) else ""
                             for i in range(component.get_num_materials())]
                for index, placement in enumerate(transforms):
                    report["instances"].append({"actor": actor.get_name(), "label": actor.get_actor_label(),
                        "component": component.get_name(), "instance": index, "mesh": key,
                        "materials": materials, "transform": transform(placement),
                        "collision": str(component.get_collision_enabled()),
                        "mobility": str(component.get_editor_property("mobility"))})
        report["status"] = "complete"
    except Exception:
        report["status"] = "failed"
        report["errors"].append(traceback.format_exc())
    finally:
        (output / "level.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if report["errors"]:
        raise RuntimeError(report["errors"][-1])
    unreal.log(f"VL GASP level export: {len(report['meshes'])} meshes, {len(report['instances'])} instances")


if __name__ == "__main__":
    main()
