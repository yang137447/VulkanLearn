#!/usr/bin/env python3
"""Build runtime manifests and copy referenced textures for the 3d66 showcase."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
from collections import defaultdict
from pathlib import Path


TEXTURE_ROLE = {
    "diffuse": "diffuse",
    "baseColor": "diffuse",
    "normals": "normal",
    "normalCamera": "normal",
    "opacity": "opacity",
    "emissive": "emissive",
    "emissionColor": "emissive",
    "metalness": "pbr",
    "diffuseRoughness": "pbr",
    "ambientOcclusion": "pbr",
}


def write_csv(path: Path, fieldnames: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def parse_args() -> argparse.Namespace:
    code_root = Path(__file__).resolve().parents[2]
    default_assets = code_root.parent / "VukanLearnResources"
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-root", type=Path, default=default_assets)
    parser.add_argument(
        "--source-texture-dir",
        type=Path,
        default=code_root / "artifacts" / "incoming" / "2_export" / "2",
    )
    parser.add_argument(
        "--source-index",
        type=Path,
        default=code_root / "artifacts" / "incoming" / "fbx_index",
    )
    parser.add_argument(
        "--split-index",
        type=Path,
        default=code_root / "artifacts" / "incoming" / "split_index",
    )
    parser.add_argument(
        "--groups-raw",
        type=Path,
        default=None,
        help="groups_raw.csv produced by split_3d66_blender.py",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    assets_root = args.assets_root.resolve()
    showcase_root = assets_root / "Maps" / "SC_cloth_showcase"
    groups_raw_path = args.groups_raw or showcase_root / "groups_raw.csv"
    texture_output_dir = showcase_root / "Source" / "Textures"
    texture_output_dir.mkdir(parents=True, exist_ok=True)

    groups_raw = read_csv(groups_raw_path)
    source_materials = {
        row["material_name"]: row for row in read_csv(args.source_index / "materials.csv")
    }
    source_textures: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in read_csv(args.source_index / "textures.csv"):
        source_textures[row["material_name"]].append(row)

    group_rows = []
    material_rows = []
    texture_rows = []
    manifest_groups = []
    missing_materials = set()
    missing_textures = set()
    copied_textures = set()

    for group in sorted(groups_raw, key=lambda item: item["id"]):
        group_id = f"{int(group['id']):03d}"
        split_name = f"SM_cloth_{group_id}"
        split_materials = read_csv(args.split_index / split_name / "materials.csv")
        group_rows.append(
            {
                "id": group_id,
                "root_name": group["source_group"],
                "kind": "group",
                "object_count": group["object_count"],
                "material_count": len(split_materials),
                "fbx": group["fbx"],
            }
        )

        manifest_materials = []
        for material in split_materials:
            source_index = int(material["material_index"])
            runtime_index = source_index + 1
            runtime_tag = f"{runtime_index:02d}"
            material_name = material["material_name"]
            source_material = source_materials.get(material_name)
            if source_material is None:
                missing_materials.add(material_name)
                diffuse = ("", "", "")
            else:
                diffuse = (
                    source_material["diffuse_r"],
                    source_material["diffuse_g"],
                    source_material["diffuse_b"],
                )
            material_rows.append(
                {
                    "id": group_id,
                    "material_index": runtime_tag,
                    "material_name": material_name,
                    "normalized_name": material_name,
                    "diffuse_r": diffuse[0],
                    "diffuse_g": diffuse[1],
                    "diffuse_b": diffuse[2],
                }
            )

            texture_manifest = []
            ordinal_by_role: dict[str, int] = defaultdict(int)
            for texture in source_textures.get(material_name, []):
                role = TEXTURE_ROLE.get(texture["type"])
                if role is None:
                    continue
                ordinal_by_role[role] += 1
                ordinal = ordinal_by_role[role]
                source_path = Path(texture["path"])
                if not source_path.is_absolute():
                    source_path = args.source_texture_dir / source_path
                if not source_path.is_file():
                    missing_textures.add(str(source_path))
                    continue
                suffix = source_path.suffix.lower() or ".png"
                normalized = (
                    f"T_cloth_{group_id}_m{runtime_tag}_{role}_{ordinal:02d}{suffix}"
                )
                target = texture_output_dir / normalized
                if normalized not in copied_textures:
                    shutil.copyfile(source_path, target)
                    copied_textures.add(normalized)
                texture_rows.append(
                    {
                        "id": group_id,
                        "material_index": runtime_tag,
                        "material_name": material_name,
                        "slot_index": str(len(texture_manifest) + 1),
                        "slot_name": role,
                        "source": str(source_path),
                        "normalized": normalized,
                    }
                )
                texture_manifest.append(
                    {
                        "role": role,
                        "source": texture["path"],
                        "normalized": normalized,
                    }
                )

            manifest_materials.append(
                {
                    "sourceIndex": source_index,
                    "runtimeIndex": runtime_index,
                    "sourceName": material_name,
                    "textures": texture_manifest,
                }
            )

        manifest_groups.append(
            {
                "id": group_id,
                "sourceGroup": group["source_group"],
                "fbx": group["fbx"],
                "materials": manifest_materials,
            }
        )

    write_csv(
        showcase_root / "groups.csv",
        ["id", "root_name", "kind", "object_count", "material_count", "fbx"],
        group_rows,
    )
    write_csv(
        showcase_root / "materials.csv",
        [
            "id",
            "material_index",
            "material_name",
            "normalized_name",
            "diffuse_r",
            "diffuse_g",
            "diffuse_b",
        ],
        material_rows,
    )
    write_csv(
        showcase_root / "textures.csv",
        [
            "id",
            "material_index",
            "material_name",
            "slot_index",
            "slot_name",
            "source",
            "normalized",
        ],
        texture_rows,
    )

    (showcase_root / "source_mapping.json").write_text(
        json.dumps(
            {
                "assetVersion": 1,
                "sourceFbx": "3d66.com_23736651.fbx",
                "groups": manifest_groups,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print(
        f"groups={len(group_rows)} materials={len(material_rows)} "
        f"textures={len(texture_rows)} copied={len(copied_textures)}"
    )
    if missing_materials:
        print(f"missing source materials={len(missing_materials)}")
        for name in sorted(missing_materials):
            print(f"  {name}")
    if missing_textures:
        print(f"missing source textures={len(missing_textures)}")
        for path in sorted(missing_textures):
            print(f"  {path}")
    return 1 if missing_materials or missing_textures else 0


if __name__ == "__main__":
    raise SystemExit(main())
