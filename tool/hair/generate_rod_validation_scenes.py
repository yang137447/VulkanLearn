#!/usr/bin/env python3
"""Regenerate the derived rod smoke scenes: N guides, capsule contacts only.

The tracked acceptance scenes under `Maps/SC_tressfx_rod_validation/` simulate 2000 guides
with body-mesh SDF contact, which is slow to iterate on. The cheap variants used for smoke
checks carry fewer guides and drop the SDF proxy, and they live in the gitignored
`Generated/Validation/` tree, so a fresh clone cannot run them. This script rebuilds the
whole family from the tracked sources.

Two derived artefacts per guide count:
  Generated/Validation/TF_female_rod_v3_<N>.json    groom descriptor restricted to N guides
  Generated/Validation/SC_rod_v3_capsules_<N>.json  scene with capsule contacts only

Dropping `hairCollision.mesh` is what selects the non-SDF pipeline, so these scenes
isolate the capsule contact path. They are smoke/iteration scenes, NOT acceptance gates;
the gates are the tracked `SC_tressfx_rod_{static,animated}.json` and
`SC_tressfx_rod_sdf_{static,animated}.json`.

Usage:
  python tool/hair/generate_rod_validation_scenes.py --resource-root ../VukanLearnResources
  python tool/hair/generate_rod_validation_scenes.py --resource-root ... --guide-counts 16 2000
"""
import argparse
import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_scene_schema import check, load_schema

DEFAULT_GUIDE_COUNTS = (16, 32, 64, 128, 200, 400, 800, 1000, 1200, 1400, 1600, 1800, 2000)
ROD_OBJECT = "Female_rod_validation"
OUTPUT_DIR = Path("Generated/Validation")

# Pinned so a smoke scene cannot drift onto another solver just because the tracked
# descriptor changed; these are the values the measured baselines were taken with.
PINNED_DEFAULTS = {
    "enabled": True,
    "backend": "gpu",
    "solver": "cosseratImplicitV3",
    "solverProfile": "effectiveBundleV1",
}


def build_mesh_descriptor(tracked, count):
    descriptor = copy.deepcopy(tracked)
    descriptor["name"] = "rod v3 %d guides" % count
    descriptor["description"] = (
        "派生冒烟资产：真实 Female groom 的前 %d 根 guide，每根 32 点，求解器固定为 "
        "cosseratImplicitV3（GPU 10 自由度 KKT + 胶囊接触）。由 "
        "tool/hair/generate_rod_validation_scenes.py 从 Maps/SC_tressfx_rod_validation/ 的受版本"
        "描述符生成，仅用于快速冒烟；验收场景是 Maps 下那四个受版本控制的场景。" % count)
    descriptor["simulationGuideCount"] = count
    descriptor["simulationDefaults"] = copy.deepcopy(PINNED_DEFAULTS)
    return descriptor


def build_scene(tracked, count, capsules):
    scene = copy.deepcopy(tracked)
    scene["name"] = "rod v3 %d guides, capsules only" % count
    scene["description"] = (
        "派生冒烟场景：前 %d 根 guide + 仅胶囊接触（去掉 hairCollision.mesh 即选中非 SDF 管线），"
        "用于快速迭代，不是验收场景；验收请看 Maps/SC_tressfx_rod_validation/ 下的四个受版本控制"
        "场景。由 tool/hair/generate_rod_validation_scenes.py 生成。" % count)
    for entry in scene.get("objects", []):
        if entry.get("name") != ROD_OBJECT:
            continue
        entry["modelPath"] = (OUTPUT_DIR / ("TF_female_rod_v3_%d.json" % count)).as_posix()
        tressfx = entry.get("tressfx") or {}
        # Capsules only: keeping the mesh reference would select the SDF pipeline instead.
        tressfx["hairCollision"] = {"capsules": copy.deepcopy(capsules)}
        entry["tressfx"] = tressfx
    return scene


def extract_capsules(tracked):
    for entry in tracked.get("objects", []):
        if entry.get("name") != ROD_OBJECT:
            continue
        collision = (entry.get("tressfx") or {}).get("hairCollision") or {}
        capsules = collision.get("capsules")
        if isinstance(capsules, list) and capsules:
            return capsules
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--guide-counts", type=int, nargs="*", default=None)
    parser.add_argument("--report", type=Path, default=None,
                        help="optional JSON report of what was written")
    options = parser.parse_args()

    resources = options.resource_root.resolve()
    case_root = resources / "Maps/SC_tressfx_rod_validation"
    tracked_mesh_path = case_root / "Meshes/TF_female_rod_validation.json"
    tracked_scene_path = case_root / "SC_tressfx_rod_static.json"
    for path in (tracked_mesh_path, tracked_scene_path):
        if not path.is_file():
            raise SystemExit("missing tracked source: %s" % path)

    tracked_mesh = json.loads(tracked_mesh_path.read_text(encoding="utf-8"))
    tracked_scene = json.loads(tracked_scene_path.read_text(encoding="utf-8"))
    capsules = extract_capsules(tracked_scene)
    if not capsules:
        raise SystemExit("the tracked scene carries no capsules; refusing to build a "
                         "capsule-only scene")

    scene_schema = load_schema("scene.schema.json")
    counts = options.guide_counts if options.guide_counts else list(DEFAULT_GUIDE_COUNTS)
    records = []
    for count in counts:
        mesh = build_mesh_descriptor(tracked_mesh, count)
        scene = build_scene(tracked_scene, count, capsules)
        problems = check(scene, scene_schema, scene_schema)
        if problems:
            raise RuntimeError("Scene schema rejected guide count %d: %s" % (count, problems))

        mesh_path = resources / OUTPUT_DIR / ("TF_female_rod_v3_%d.json" % count)
        scene_path = resources / OUTPUT_DIR / ("SC_rod_v3_capsules_%d.json" % count)
        mesh_path.parent.mkdir(parents=True, exist_ok=True)
        mesh_path.write_text(json.dumps(mesh, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8", newline="\n")
        scene_path.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n",
                              encoding="utf-8", newline="\n")
        records.append({"guideCount": count,
                        "mesh": str(mesh_path.relative_to(resources)),
                        "scene": str(scene_path.relative_to(resources)),
                        "capsules": len(capsules),
                        "schemaProblems": problems})
        print("wrote %s and %s" % (mesh_path.name, scene_path.name))

    if options.report:
        options.report.parent.mkdir(parents=True, exist_ok=True)
        options.report.write_text(json.dumps({"records": records}, ensure_ascii=False, indent=2) + "\n",
                                  encoding="utf-8", newline="\n")
    print("\n%d guide-count variants refreshed under %s"
          % (len(counts), resources / OUTPUT_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
