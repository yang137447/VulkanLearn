"""Read-only inspection of the UE Third Person Template AnimBlueprint.

Run inside an isolated copy of a UE Third Person Template project with UE's
PythonScript commandlet. The script never saves a package or upgrades the
project. Set VL_THIRD_PERSON_AUDIT_OUTPUT to a new directory outside the UE
project.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import traceback

import unreal


ROOT = "/Game/Characters/Mannequins"
ANIM_BP = ROOT + "/Animations/ABP_Manny"
ALT_ANIM_BP = ROOT + "/Animations/ABP_Quinn"
CHARACTER_BP = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"
GAME_MODE_BP = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonGameMode"
WALK_RUN_BLEND = ROOT + "/Animations/Manny/BS_MM_WalkRun"
QUINN_WALK_RUN_BLEND = ROOT + "/Animations/Quinn/BS_MF_Unarmed_WalkRun"
CLIPS = (
    ROOT + "/Animations/Manny/MM_Idle",
    ROOT + "/Animations/Manny/MM_Walk_Fwd",
    ROOT + "/Animations/Manny/MM_Run_Fwd",
    ROOT + "/Animations/Manny/MM_Jump",
    ROOT + "/Animations/Manny/MM_Fall_Loop",
    ROOT + "/Animations/Manny/MM_Land",
)


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def serialize(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, unreal.Object):
        return {"path": value.get_path_name(), "class": value.get_class().get_name()}
    if isinstance(value, unreal.StructBase):
        return {"ueStructText": value.export_text()}
    if isinstance(value, (unreal.Array, list, tuple)):
        return [serialize(item) for item in value]
    return str(value)


def safe_properties(obj, names):
    result = {}
    for name in names:
        try:
            result[name] = serialize(obj.get_editor_property(name))
        except Exception as error:
            result[name] = {"unavailable": str(error)}
    return result


def interesting_names(obj):
    try:
        names = dir(obj)
    except Exception:
        return []
    keys = ("fade", "state", "transition", "graph", "rule", "condition",
            "blend", "duration", "priority", "enabled", "cross", "machine")
    return [name for name in names if any(key in name.lower() for key in keys)]


def export_file(obj, path: Path, exporter) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.set_editor_property("object", obj)
    task.set_editor_property("filename", str(path))
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("exporter", exporter)
    try:
        success = bool(unreal.Exporter.run_asset_export_task(task))
    except Exception as error:
        return {"file": str(path), "success": False, "error": str(error)}
    size = path.stat().st_size if path.exists() else 0
    return {"file": str(path), "success": success and size > 0, "bytes": size}


def export_text(obj, path: Path) -> dict:
    try:
        text = obj.export_text()
    except Exception as error:
        return {"file": str(path), "success": False, "error": str(error)}
    if not text:
        return {"file": str(path), "success": False, "error": "empty export_text"}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return {"file": str(path), "success": True, "bytes": len(text.encode("utf-8"))}


def graph_nodes(graph):
    try:
        nodes = graph.get_editor_property("nodes")
    except Exception:
        try:
            nodes = graph.get_editor_property("Nodes")
        except Exception as error:
            return {"error": str(error)}
    result = []
    for node in nodes:
        record = {
            "name": node.get_name(),
            "class": node.get_class().get_name(),
            "properties": safe_properties(node, (
                "node_comment", "cross_fade_duration", "blend_mode",
                "logic_type", "priority_order", "b_transition_enabled",
                "transition_enabled", "crossfade_duration",
                "automatic_rule", "can_enter_transition", "state_name",
            )),
            "interesting": interesting_names(node),
        }
        try:
            record["text"] = node.export_text()
        except Exception as error:
            record["text"] = {"unavailable": str(error)}
        result.append(record)
    return result


def graph_record(graph):
    record = {
        "name": graph.get_name(),
        "class": graph.get_class().get_name(),
        "properties": safe_properties(graph, (
            "schema", "graph_type", "nodes", "nodes_inner",
        )),
        "interesting": interesting_names(graph),
    }
    nodes = graph_nodes(graph)
    record["nodes"] = nodes if isinstance(nodes, dict) else nodes
    return record


def collect_graphs(obj):
    result = []
    # UAnimBlueprint 的 Python 包装只暴露 get_animation_graphs()，普通
    # get_editor_property('function_graphs') 在 UE 5.8 会报属性不存在。
    try:
        value = obj.get_animation_graphs()
    except Exception:
        value = None
    if isinstance(value, (list, unreal.Array)):
        for graph in value:
            result.append(("animation_graphs", graph))
    elif isinstance(value, unreal.Object):
        result.append(("animation_graphs", value))
    for property_name in (
            "function_graphs", "ubergraph_pages", "macro_graphs",
            "delegate_signature_graphs", "event_graphs", "anim_graph"):
        try:
            value = obj.get_editor_property(property_name)
        except Exception:
            continue
        if isinstance(value, (list, unreal.Array)):
            for graph in value:
                result.append((property_name, graph))
        elif isinstance(value, unreal.Object):
            result.append((property_name, value))
    return result


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    output = Path(os.environ["VL_THIRD_PERSON_AUDIT_OUTPUT"].strip()).resolve()
    if output == project or project in output.parents:
        raise RuntimeError("Audit output must be outside the UE project")
    if output.exists():
        raise RuntimeError("Audit output already exists: " + str(output))
    output.mkdir(parents=True)

    report = {
        "format": "vl.third-person.anim-audit.v1",
        "engine": unreal.SystemLibrary.get_engine_version(),
        "project": str(project),
        "assets": {},
        "errors": [],
        "scope": "Read-only graph/property inspection; no package save.",
    }
    try:
        for path in (ANIM_BP, ALT_ANIM_BP, CHARACTER_BP, GAME_MODE_BP,
                     WALK_RUN_BLEND, QUINN_WALK_RUN_BLEND) + CLIPS:
            asset = unreal.load_asset(path)
            if asset is None:
                report["errors"].append({"asset": path, "error": "not found"})
                continue
            key = path.rsplit("/", 1)[-1]
            data = {
                "path": path,
                "class": asset.get_class().get_name(),
                "interesting": interesting_names(asset),
                "properties": safe_properties(asset, (
                    "skeleton", "preview_mesh", "parent_class", "generated_class",
                    "anim_graph", "function_graphs", "ubergraph_pages",
                    "macro_graphs", "baked_state_machines", "state_machines",
                    "baked_state_machine", "status",
                )),
                "textExport": export_file(
                    asset, output / (key + ".t3d"), unreal.ObjectExporterT3D()),
                "graphs": [],
            }
            try:
                generated = asset.get_editor_property("generated_class")
                data["generated"] = {
                    "class": generated.get_class().get_name(),
                    "interesting": interesting_names(generated),
                    "properties": safe_properties(generated, (
                        "baked_state_machines", "state_machines",
                        "baked_state_machine", "anim_node_data",
                    )),
                    "textExport": export_file(
                        generated, output / (key + ".generated.t3d"),
                        unreal.ObjectExporterT3D()),
                }
            except Exception as error:
                data["generated"] = {"unavailable": str(error)}
            for property_name, graph in collect_graphs(asset):
                graph_name = property_name + "_" + graph.get_name()
                record = graph_record(graph)
                record["textExport"] = export_text(
                    graph, output / "graphs" / (graph_name + ".txt"))
                record["t3dExport"] = export_file(
                    graph, output / "graphs" / (graph_name + ".t3d"),
                    unreal.ObjectExporterT3D())
                data["graphs"].append(record)
            report["assets"][key] = data
    except Exception:
        report["errors"].append({"error": traceback.format_exc()})
    finally:
        write_json(output / "anim-audit.json", report)
    unreal.log("[VL Third Person Audit] " + str(output))


if __name__ == "__main__":
    main()
