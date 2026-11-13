"""Read-only dump of ABP_Manny ApplyAdditive/Slot/CachedPose node properties.

Run inside UE's PythonScript commandlet; writes only to VL_ADDITIVE_AUDIT_OUTPUT.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


ROOT = "/Game/Characters/Mannequins"
ANIM_BP = ROOT + "/Animations/ABP_Manny"
ANIMS = ROOT + "/Animations/Manny"


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def node_text(obj) -> str:
    try:
        node = obj.get_editor_property("node")
        return node.export_text()
    except Exception as error:
        return "unavailable: " + str(error)


def walk_graph(graph, depth, output):
    if depth > 12 or graph is None:
        return
    try:
        nodes = list(graph.get_editor_property("nodes"))
    except Exception as error:
        output.append({"graph": graph.get_path_name(), "error": str(error)})
        return
    for node in nodes:
        name = node.get_class().get_name()
        if name in ("AnimGraphNode_ApplyAdditive", "AnimGraphNode_Slot",
                    "AnimGraphNode_SaveCachedPose", "AnimGraphNode_UseCachedPose"):
            output.append({
                "path": node.get_path_name(),
                "class": name,
                "properties": {
                    "node": node_text(node),
                    "slot_name": str(node.get_editor_property("slot_name"))
                    if name == "AnimGraphNode_Slot" else None,
                },
            })
        for property_name in ("bound_graph", "state_entered_event", "sub_graphs"):
            try:
                sub = node.get_editor_property(property_name)
            except Exception:
                continue
            if isinstance(sub, (list, tuple)):
                for item in sub:
                    walk_graph(item, depth + 1, output)
            else:
                walk_graph(sub, depth + 1, output)


def main():
    output_dir = Path(os.environ["VL_ADDITIVE_AUDIT_OUTPUT"].strip()).resolve()
    report = {"asset": ANIM_BP, "nodes": [], "clips": [], "errors": []}
    try:
        asset = unreal.load_asset(ANIM_BP)
        graph_paths = (
            "AnimGraph.AnimGraphNode_SaveCachedPose_0",
            "AnimGraph.AnimGraphNode_Slot_0",
            "AnimGraph.AnimGraphNode_StateMachine_1.Main States.AnimStateNode_3.Land.AnimGraphNode_ApplyAdditive_1",
            "AnimGraph.AnimGraphNode_StateMachine_1.Main States.AnimStateNode_3.Land.AnimGraphNode_UseCachedPose_0",
            "AnimGraph.AnimGraphNode_StateMachine_1.Main States.AnimStateNode_0.Locomotion.AnimGraphNode_UseCachedPose_0",
        )
        base = ANIM_BP + "." + ANIM_BP.rsplit("/", 1)[-1]
        for relative in graph_paths:
            obj = unreal.load_object(None, base + ":" + relative)
            if obj is None:
                report["errors"].append("not found: " + relative)
                continue
            record = {"path": obj.get_path_name(),
                      "class": obj.get_class().get_name(),
                      "node": node_text(obj)}
            for property_name in ("slot_name", "cached_pose_name", "pose_name"):
                try:
                    record[property_name] = str(obj.get_editor_property(property_name))
                except Exception:
                    pass
            report["nodes"].append(record)
        for path in unreal.EditorAssetLibrary.list_assets(ANIMS, recursive=False):
            report["clips"].append(str(path))
    except Exception as error:
        report["errors"].append(str(error))
    finally:
        write_json(output_dir / "additive-audit.json", report)


if __name__ == "__main__":
    main()
