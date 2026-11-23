"""Normalize only new V2 material defaults; audit exact Paint source/copy state."""

import argparse
import json
import os
from pathlib import Path
import tempfile

from build_composited_fixture import read_json, reference_path, require_topic_root, validate_author_world
from rvt_author_semantics import author_parameter, remove_default_parameters, paint_semantics
from rvt_delivery import artifact


def atomic_json(path, document):
    descriptor, temporary = tempfile.mkstemp(prefix=".normalize-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation-receipt", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if options.output.exists():
        raise ValueError("Preserve prior normalization receipt")
    code = Path(__file__).resolve().parents[3]
    resources = require_topic_root(code)
    schema_path = code / "shader/glsl/M_terrain.json"
    schema = read_json(schema_path)
    initial = read_json(options.generation_receipt)
    protected = {}
    changes, states, semantics = [], [], {}
    for mode, generated in initial["generation"].items():
        scene_name = generated["scene"]
        directory = reference_path(resources, scene_name).parent
        if directory.name != f"RVTv2_Full4x4_R8192_Default_{mode}":
            raise ValueError("Normalization restricted to the two exact new V2 namespaces")
        world = validate_author_world(resources, scene_name)
        sources = {row["tile"]: row["sourceTerrain"] for row in generated["tiles"]}
        records = []
        for entry in world["terrains"]:
            placement, terrain = entry["placement"], entry["terrain"]
            tile = placement["name"].split("_")[-1]
            source_name = sources[tile]
            source = read_json(reference_path(resources, source_name))
            for name in [source_name, *(source[field] for field in (
                    "heightmapPath", "paintPath", "materialInstancePath", "detailMaterialInstancePath")
                    if source.get(field))]:
                protected[name] = artifact(reference_path(resources, name))
            for field in ("heightmapPath", "paintPath"):
                if terrain.get(field):
                    protected[terrain[field]] = artifact(reference_path(resources, terrain[field]))
                    for name in (source[field] + ".meta.json", terrain[field] + ".meta.json"):
                        if (resources / name).is_file():
                            protected[name] = artifact(reference_path(resources, name))
            for field in ("materialInstancePath", "detailMaterialInstancePath"):
                name = terrain[field]
                path = reference_path(resources, name)
                material = read_json(path)
                parameter = "u_terrainWeightMapMode"
                desired = 0 if mode == "CoverageAtlas" else 1
                actual = material.get("parameters", {}).get(parameter, schema["parameters"][parameter]["default"])
                if type(actual) not in (int, float) or actual != desired:
                    raise ValueError("Existing author mode changed semantically; do not overwrite")
                before = artifact(path)
                before_parameters = dict(material.get("parameters", {}))
                author_parameter(material, schema, parameter, desired)
                removed = remove_default_parameters(material, schema)
                if before_parameters != material.get("parameters", {}):
                    atomic_json(path, material)
                    changes.append({"path": name, "before": before, "after": artifact(path),
                                    "parametersBefore": before_parameters,
                                    "parametersAfter": material.get("parameters", {}),
                                    "reason": "Exact current M_terrain defaults omitted; effective values unchanged"})
            current = paint_semantics(resources, terrain, placement["terrainPath"])
            original = paint_semantics(resources, source, source_name)
            if original["paintSha256"] != current["paintSha256"] or original["layerKeys"] != current["layerKeys"]:
                raise ValueError("New author Paint does not match preserved original semantics")
            if original["sidecarExists"] and not current["sidecarExists"]:
                raise ValueError("Existing source Paint sidecar was lost from author copy")
            states.append({"mode": mode, "surfaceId": "terrain|" + placement["name"],
                           "source": original, "copy": current,
                           "sourceCopyExistenceMatches": original["sidecarExists"] == current["sidecarExists"],
                           "paintByteIdentityVerified": True})
            records.append(current)
        semantic_path = directory / "paint_semantics.json"
        atomic_json(semantic_path, {"kind": "terrain-author-paint-semantics", "version": 1,
            "surfaces": records, "scope": "Complete VLP1+TR layer identity; legacy absence explicit; frozen V2 effective-weight objects must be verified after Cook"})
        semantics[mode] = artifact(semantic_path)
    if protected != {name: artifact(reference_path(resources, name)) for name in protected}:
        raise ValueError("Protected old author or new Height/Paint changed during normalization")
    report = {"status": "new-v2-defaults-normalized-not-cooked", "schema": artifact(schema_path),
              "generationReceipt": artifact(options.generation_receipt), "changes": changes,
              "changedMaterialCount": len(changes), "paintSidecarAudit": states,
              "paintSemanticsReceipts": semantics, "protectedInputs": protected,
              "rendererStarted": False, "cookStarted": False, "cookPassed": False, "acceptancePassed": False}
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"status": report["status"], "changedMaterialCount": len(changes),
                      "paintSurfacesAudited": len(states),
                      "legacyPaintSourcesWithoutSidecar": sum(not row["source"]["sidecarExists"] for row in states)},
                     indent=2))


if __name__ == "__main__":
    main()
