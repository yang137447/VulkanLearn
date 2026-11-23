"""Read-only V2 World author preflight; never starts renderer or invents budgets."""

import argparse
from collections import Counter
import json
from pathlib import Path

from build_composited_fixture import V2_SCENE, read_json, reference_path, require_topic_root, validate_author_world
from rvt_delivery import load_adapter, validate_frozen
from rvt_generation_identity import COOK_IDENTITY, RUNTIME_COMPUTES
from rvt_author_semantics import remove_default_parameters, paint_semantics

ROOT = Path(__file__).resolve().parents[3]
RETIRED = {"u_terrainLayerMode", "u_layerForceMip", "u_layerPageResolution",
           "u_layerAtlasResolution", "u_layerPagesPerAtlasSlice", "u_layerTilesPerAtlasSide",
           "u_layerMaxMip", "u_heightBlendDistanceFade", "u_compositedEnabled",
           "u_terrainHeightAtlasEnabled"}


def preflight(resources, scene, schema, capture, frozen=None):
    world = validate_author_world(resources, scene)
    if capture.get("requiredCookIdentity") != COOK_IDENTITY:
        raise ValueError("Author fixture requires current gen3 padding Cook identity; no gen2 page reuse")
    modes = set()
    paints = []
    for entry in world["terrains"]:
        paints.append(paint_semantics(resources, entry["terrain"], entry["placement"]["terrainPath"]))
        for field in ("materialInstancePath", "detailMaterialInstancePath"):
            material = read_json(reference_path(resources, entry["terrain"][field]))
            parameters = material.get("parameters", {})
            if RETIRED & parameters.keys() or not parameters.keys() <= schema["parameters"].keys():
                raise ValueError(f"Retired/unknown material parameter: {entry['placement']['name']}")
            if not material.get("textures", {}).keys() <= schema["textures"].keys():
                raise ValueError("Unknown terrain texture slot")
            redundant = remove_default_parameters({"parameters": dict(parameters)}, schema)
            if redundant:
                raise ValueError(f"Redundant default material parameters require author normalization: {sorted(redundant)}")
            mode = parameters.get("u_terrainWeightMapMode",
                                  schema["parameters"]["u_terrainWeightMapMode"]["default"])
            if type(mode) not in (int, float) or mode not in (0, 1):
                raise ValueError("Unsupported weight mode")
            modes.add("CoverageAtlas" if mode == 0 else "LocalLayerArray")
    counts = Counter(shot.get("category") for shot in capture["shots"] if shot.get("category"))
    if counts != {"center": 16, "edge": 24, "junction": 9, "outer": 4}:
        raise ValueError("Full16 needs 16 centers, 24 edges, 9 junctions and four outer corners")
    return {"status": "prepared-only" if frozen else "incomplete",
            "scene": scene, "surfaceIds": world["surface_ids"], "worldManifest": world["manifest"],
            "productionSourcePath": world["recipe"], "weightSources": sorted(modes),
            "requiredCookIdentity": COOK_IDENTITY, "requiredRuntimeComputes": RUNTIME_COMPUTES,
            "paintAuthorInputs": paints,
            "paintProductionClosure": "pending actual Cook: verify complete coverage/lossless local-weight objects and layer mapping; legacy source sidecar absence is not repaired by invented metadata",
            "cameraCounts": dict(counts), "budgets": frozen["budgets"] if frozen else None,
            "pending": ["Runtime allocation admission", "GPU/visual/offline delivery evidence"] +
                       ([] if frozen else ["Accepted P0 freeze and real schema adapter"]),
            "rendererStarted": False, "cookStarted": False, "acceptancePassed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", default=V2_SCENE)
    parser.add_argument("--p0-freeze", type=Path)
    parser.add_argument("--adapter", type=Path)
    options = parser.parse_args()
    resources = require_topic_root(ROOT)
    frozen = None
    if options.p0_freeze or options.adapter:
        if not options.p0_freeze or not options.adapter:
            parser.error("--p0-freeze and --adapter must be supplied together")
        frozen = validate_frozen(load_adapter(options.adapter).normalize_p0(options.p0_freeze),
                                 options.p0_freeze.resolve().parent)
    result = preflight(resources, options.scene, read_json(ROOT / "shader/glsl/M_terrain.json"),
                       read_json(resources / Path(options.scene).parent / "capture_plan.json"), frozen)
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 3 if result["status"] == "incomplete" else 0


if __name__ == "__main__":
    raise SystemExit(main())
