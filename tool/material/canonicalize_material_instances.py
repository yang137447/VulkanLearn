from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any


PROPERTY_PATTERN = r'(?m)^{indent}"{name}"[ \t]*:[ \t]*'


def find_property_match(
    text: str,
    property_name: str,
    indent: str,
    start: int = 0,
    end: int | None = None,
) -> re.Match[str] | None:
    return re.search(
        PROPERTY_PATTERN.format(
            indent=re.escape(indent),
            name=re.escape(property_name),
        ),
        text[start:end],
    )


def remove_json_property(
    text: str,
    property_name: str,
    parent_name: str | None = None,
) -> str:
    top_level_match = re.search(r"\{\r?\n(?P<indent>[ \t]+)\"", text)
    if top_level_match is None:
        raise RuntimeError("JSON object is not formatted with indented properties")
    top_level_indent = top_level_match.group("indent")

    search_start = 0
    search_end = len(text)
    property_indent = top_level_indent
    if parent_name is not None:
        parent_match = find_property_match(
            text,
            parent_name,
            top_level_indent,
        )
        if parent_match is None:
            raise RuntimeError(
                f'Parent property "{parent_name}" was not found while rewriting JSON'
            )
        _, parent_value_length = json.JSONDecoder().raw_decode(
            text[parent_match.end() :]
        )
        search_start = parent_match.end()
        search_end = search_start + parent_value_length
        child_match = re.search(
            rf'(?m)^(?P<indent>[ \t]+)"{re.escape(property_name)}"[ \t]*:[ \t]*',
            text[search_start:search_end],
        )
        if child_match is not None:
            property_indent = child_match.group("indent")

    relative_match = find_property_match(
        text,
        property_name,
        property_indent,
        search_start,
        search_end,
    )
    match = None
    if relative_match is not None:
        match_start = search_start + relative_match.start()
        match_end = search_start + relative_match.end()
        match = re.match(
            PROPERTY_PATTERN.format(
                indent=re.escape(property_indent),
                name=re.escape(property_name),
            ),
            text[match_start:match_end],
        )
    if match is None:
        raise RuntimeError(f'Property "{property_name}" was not found while rewriting JSON')

    match_start = search_start + relative_match.start()
    match_end = search_start + relative_match.end()

    _, value_length = json.JSONDecoder().raw_decode(text[match_end:])
    value_end = match_end + value_length
    cursor = value_end
    while cursor < len(text) and text[cursor] in " \t":
        cursor += 1

    if cursor < len(text) and text[cursor] == ",":
        cursor += 1
        while cursor < len(text) and text[cursor] in " \t":
            cursor += 1
        if text.startswith("\r\n", cursor):
            cursor += 2
        elif cursor < len(text) and text[cursor] == "\n":
            cursor += 1
        return text[:match_start] + text[cursor:]

    if text.startswith("\r\n", cursor):
        cursor += 2
    elif cursor < len(text) and text[cursor] == "\n":
        cursor += 1

    prefix = text[:match_start]
    previous = len(prefix) - 1
    while previous >= 0 and prefix[previous].isspace():
        previous -= 1
    if previous >= 0 and prefix[previous] == ",":
        prefix = prefix[:previous] + prefix[previous + 1 :]
    return prefix + text[cursor:]


def canonicalize_instance(
    material_instance: dict[str, Any],
    material: dict[str, Any],
) -> tuple[dict[str, Any], Counter[str]]:
    result = json.loads(json.dumps(material_instance))
    changes: Counter[str] = Counter()

    authored_macros = result.get("macros", {})
    macro_defaults = material["macros"]
    unknown_macros = authored_macros.keys() - macro_defaults.keys()
    if unknown_macros:
        raise RuntimeError(f"Unknown macro overrides: {sorted(unknown_macros)}")

    for name in list(authored_macros):
        if authored_macros[name] == macro_defaults[name]:
            del authored_macros[name]
            changes["macro"] += 1

    authored_render_states = result.get("renderStateOverrides", {})
    for name in list(authored_render_states):
        default_value = (
            material["shadingModel"]
            if name == "shadingModel"
            else material["renderStates"].get(name)
        )
        if authored_render_states[name] == default_value:
            del authored_render_states[name]
            changes["renderState"] += 1

    authored_parameters = result.get("parameters", {})
    parameter_schema = material["parameters"]
    unknown_parameters = authored_parameters.keys() - parameter_schema.keys()
    if unknown_parameters:
        raise RuntimeError(f"Unknown parameter overrides: {sorted(unknown_parameters)}")
    for name in list(authored_parameters):
        descriptor = parameter_schema[name]
        if authored_parameters[name] == descriptor["default"]:
            del authored_parameters[name]
            changes["parameterDefault"] += 1

    authored_textures = result.get("textures", {})
    texture_schema = material["textures"]
    unknown_textures = authored_textures.keys() - texture_schema.keys()
    if unknown_textures:
        raise RuntimeError(f"Unknown texture overrides: {sorted(unknown_textures)}")
    for name in list(authored_textures):
        descriptor = texture_schema[name]
        if authored_textures[name] == descriptor["default"]:
            del authored_textures[name]
            changes["textureDefault"] += 1

    for section in ("renderStateOverrides", "macros", "parameters", "textures"):
        if section in result and not result[section]:
            del result[section]
            changes["emptySection"] += 1

    return result, changes


def rewrite_instance_text(
    original_text: str,
    original: dict[str, Any],
    canonical: dict[str, Any],
) -> str:
    rewritten = original_text
    for section in ("renderStateOverrides", "macros", "parameters", "textures"):
        original_values = original.get(section, {})
        canonical_values = canonical.get(section, {})
        for name in original_values.keys() - canonical_values.keys():
            rewritten = remove_json_property(rewritten, name, section)
        if section in original and section not in canonical:
            rewritten = remove_json_property(rewritten, section)

    parsed = json.loads(rewritten)
    if parsed != canonical:
        raise RuntimeError("Rewritten JSON does not match the canonical material instance")
    return rewritten


def load_material_definitions(project_root: Path) -> dict[str, dict[str, Any]]:
    definitions: dict[str, dict[str, Any]] = {}
    for path in (project_root / "shader" / "glsl").rglob("M_*.json"):
        material = json.loads(path.read_text(encoding="utf-8"))
        definitions[path.relative_to(project_root).as_posix()] = material
    return definitions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    project_root = arguments.project_root.resolve()
    resource_root = arguments.resource_root.resolve()
    material_definitions = load_material_definitions(project_root)
    changed_files = 0
    total_changes: Counter[str] = Counter()

    material_paths = list(resource_root.glob("Common/Materials/**/MI_*.json"))
    material_paths.extend(resource_root.glob("Maps/*/Materials/MI_*.json"))
    for path in sorted(material_paths):
        original_text = path.read_text(encoding="utf-8")
        material_instance = json.loads(original_text)
        material_reference = material_instance.get("material")
        if not isinstance(material_reference, str):
            raise RuntimeError(f"Material instance has no material reference: {path}")
        material = material_definitions.get(material_reference.replace("\\", "/"))
        if material is None:
            raise RuntimeError(
                f"Unknown material definition {material_reference}: {path}"
            )

        canonical, changes = canonicalize_instance(material_instance, material)
        if not changes:
            continue
        rewritten = rewrite_instance_text(original_text, material_instance, canonical)
        changed_files += 1
        total_changes.update(changes)
        if not arguments.check:
            with path.open("w", encoding="utf-8", newline="") as output:
                output.write(rewritten)

    print(
        "MATERIAL_INSTANCE_CANONICALIZE "
        f"changedFiles={changed_files} "
        + " ".join(
            f"{name}={count}" for name, count in sorted(total_changes.items())
        )
    )
    return 1 if arguments.check and changed_files else 0


if __name__ == "__main__":
    raise SystemExit(main())
