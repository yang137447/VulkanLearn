from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path
from typing import Any


JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path: Path):
    data = path.read_bytes()
    off = 12
    doc = None
    binary = None
    while off < len(data):
        n, t = struct.unpack_from("<II", data, off)
        chunk = data[off + 8 : off + 8 + n]
        if t == JSON_CHUNK:
            doc = json.loads(chunk)
        elif t == BIN_CHUNK:
            binary = bytearray(chunk)
        off += 8 + n
    if doc is None or binary is None:
        raise RuntimeError(f"Invalid GLB: {path}")
    return doc, binary


def write_glb(path: Path, doc: dict[str, Any], binary: bytearray) -> None:
    raw = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    raw += b" " * ((4 - len(raw) % 4) % 4)
    blob = bytes(binary)
    blob += b"\0" * ((4 - len(blob) % 4) % 4)
    with path.open("wb") as file:
        file.write(struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(raw) + 8 + len(blob)))
        file.write(struct.pack("<II", len(raw), JSON_CHUNK))
        file.write(raw)
        file.write(struct.pack("<II", len(blob), BIN_CHUNK))
        file.write(blob)


def group_key(name: str):
    low = name.lower()
    if "blocker" in low:
        return None

    building = re.search(r"(?:paris_)?building_?(\d+)", low)
    if building:
        return ("building", f"Building{building.group(1).zfill(2)}")
    if "aerial" in low:
        return ("category", "Aerial")
    if "ivy" in low:
        return ("category", "Vegetation")
    if "awning" in low or "banner" in low:
        return ("category", "Props")
    if "street" in low or "trafficsign" in low:
        return ("category", "Street")
    if any(token in low for token in ("hedge", "cypress", "flower", "bush", "tree")):
        return ("category", "Vegetation")
    if "chimney" in low:
        return ("category", "Props")
    if any(
        token in low
        for token in (
            "vespa",
            "ashtray",
            "odometer",
            "trash",
            "shop",
            "sign",
            "bollard",
            "electricbox",
            "menusign",
            "balcony",
            "glass",
        )
    ):
        return ("category", "Props")
    return ("category", "Other")


def split_glb(
    doc: dict[str, Any],
    binary: bytearray,
    node_indices: list[int],
) -> tuple[dict[str, Any], bytearray]:
    selected_nodes = [doc["nodes"][index] for index in node_indices]
    selected_mesh_indices = sorted({node["mesh"] for node in selected_nodes})
    mesh_index_map = {old: new for new, old in enumerate(selected_mesh_indices)}

    accessors: list[dict[str, Any]] = []
    buffer_views: list[dict[str, Any]] = []
    accessor_map: dict[int, int] = {}
    buffer_view_map: dict[int, int] = {}

    def remap_accessor(old_index: int) -> int:
        if old_index in accessor_map:
            return accessor_map[old_index]

        new_index = len(accessors)
        accessor_map[old_index] = new_index
        accessor = dict(doc["accessors"][old_index])
        if "bufferView" in accessor:
            old_view = accessor["bufferView"]
            if old_view not in buffer_view_map:
                buffer_view_map[old_view] = len(buffer_views)
                buffer_views.append(dict(doc["bufferViews"][old_view]))
            accessor["bufferView"] = buffer_view_map[old_view]
        accessors.append(accessor)
        return new_index

    materials: list[dict[str, Any]] = []
    material_map: dict[int, int] = {}
    meshes: list[dict[str, Any]] = []

    for old_mesh_index in selected_mesh_indices:
        old_mesh = doc["meshes"][old_mesh_index]
        primitives: list[dict[str, Any]] = []
        for primitive in old_mesh.get("primitives", []):
            new_primitive: dict[str, Any] = {}
            attributes = {}
            for attribute_name, accessor_index in primitive.get("attributes", {}).items():
                attributes[attribute_name] = remap_accessor(accessor_index)
            new_primitive["attributes"] = attributes
            if "indices" in primitive:
                new_primitive["indices"] = remap_accessor(primitive["indices"])
            if "mode" in primitive:
                new_primitive["mode"] = primitive["mode"]
            material_index = primitive.get("material", 0)
            if material_index not in material_map:
                material_map[material_index] = len(materials)
                materials.append(dict(doc["materials"][material_index]))
            new_primitive["material"] = material_map[material_index]
            primitives.append(new_primitive)

        new_mesh: dict[str, Any] = {"primitives": primitives}
        if "name" in old_mesh:
            new_mesh["name"] = old_mesh["name"]
        meshes.append(new_mesh)

    nodes: list[dict[str, Any]] = []
    for node in selected_nodes:
        new_node: dict[str, Any] = {
            "name": node.get("name", ""),
            "mesh": mesh_index_map[node["mesh"]],
        }
        for key in ("translation", "rotation", "scale", "matrix"):
            if key in node:
                new_node[key] = node[key]
        nodes.append(new_node)

    new_doc: dict[str, Any] = {
        "asset": doc.get("asset", {"version": "2.0"}),
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": doc.get("buffers", []),
    }
    return new_doc, binary


def split_models(resources: Path) -> dict[str, list[tuple[str, str]]]:
    model_output = resources / "models/bistro_modular"
    data_output = resources / "models/datas/bistro_modular"
    scene_path = resources / "scenes/SC_bistro_exterior_modular.json"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))

    old_model_paths: dict[str, str] = {}
    model_to_objects: dict[str, list[dict[str, Any]]] = {}
    for obj in scene.get("objects", []):
        if obj.get("type") != "mesh":
            continue
        model_path = obj.get("modelPath", "")
        model_to_objects.setdefault(model_path, []).append(obj)

    replacements: dict[str, list[tuple[str, str]]] = {}
    for glb in sorted(data_output.rglob("*.glb")):
        if "FillOut" in glb.parts:
            continue

        relative_stem = "_".join(glb.relative_to(data_output).with_suffix("").parts)
        old_sm = model_output / f"SM_{relative_stem}.json"
        if not old_sm.exists():
            continue
        old_descriptor = json.loads(old_sm.read_text(encoding="utf-8"))
        slot_paths = {
            slot["name"]: slot["materialInstancePath"]
            for slot in old_descriptor.get("materialSlots", [])
        }

        doc, binary = read_glb(glb)
        groups: dict[tuple[str, str], list[int]] = {}
        for index, node in enumerate(doc.get("nodes", [])):
            if "mesh" not in node:
                continue
            key = group_key(node.get("name", ""))
            if key is None:
                continue
            groups.setdefault(key, []).append(index)

        if len(groups) <= 1:
            old_model_path = f"models/bistro_modular/SM_{relative_stem}.json"
            old_model_paths[str(glb)] = old_model_path
            continue

        new_models: list[tuple[str, str]] = []
        relative_dir = glb.relative_to(data_output).parent
        for (kind, group_name), node_indices in sorted(groups.items()):
            split_doc, split_binary = split_glb(doc, binary, node_indices)
            suffix = group_name if kind == "building" else group_name
            data_name = f"{glb.stem}_{suffix}.glb"
            data_rel = relative_dir / data_name
            split_glb_path = data_output / data_rel
            split_glb_path.parent.mkdir(parents=True, exist_ok=True)
            write_glb(split_glb_path, split_doc, split_binary)

            used_material_names = []
            for material in split_doc.get("materials", []):
                name = material.get("name", "")
                if name not in slot_paths:
                    continue
                used_material_names.append(name)

            descriptor_name = f"SM_{relative_stem}_{suffix}"
            descriptor_path = model_output / f"{descriptor_name}.json"
            descriptor = {
                "name": descriptor_name,
                "type": "mesh",
                "modelDataPath": f"models/datas/bistro_modular/{data_rel.as_posix()}",
                "materialSlots": [
                    {"name": name, "materialInstancePath": slot_paths[name]}
                    for name in used_material_names
                ],
            }
            descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")
            new_models.append(
                (
                    f"models/bistro_modular/{descriptor_name}.json",
                    f"{data_rel.stem}",
                )
            )
        replacements[f"models/bistro_modular/SM_{relative_stem}.json"] = new_models

    new_objects: list[dict[str, Any]] = []
    for obj in scene.get("objects", []):
        if obj.get("type") != "mesh":
            new_objects.append(obj)
            continue
        model_path = obj.get("modelPath", "")
        replacement = replacements.get(model_path)
        if not replacement:
            new_objects.append(obj)
            continue
        base_name = obj.get("name", "")
        for new_model_path, suffix in replacement:
            copy = dict(obj)
            copy["modelPath"] = new_model_path
            copy["name"] = f"{base_name}_{suffix}"
            new_objects.append(copy)

    scene["objects"] = new_objects
    scene_path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")

    counts = {old: len(new) for old, new in replacements.items()}
    print(f"split {len(replacements)} GLBs; new model paths={sum(counts.values())}")
    return replacements


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Split mixed Bistro GLBs into logical game-scene model assets."
    )
    parser.add_argument("--resources", type=Path, required=True)
    args = parser.parse_args()
    split_models(args.resources.resolve())


if __name__ == "__main__":
    main()
