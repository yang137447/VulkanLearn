"""Prepare a disjoint real Pegasus source closure for bounded P0 GPU timing."""

import argparse
import json
from pathlib import Path
import shutil


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--mode", choices=("coverage", "local"), required=True)
    parser.add_argument("--refresh-owned-input", action="store_true")
    parser.add_argument("--resolution", type=int, choices=(1024, 8192), default=1024)
    options = parser.parse_args()
    root = options.destination.resolve()
    if options.refresh_owned_input:
        if read(root / "p0-producer-input.json")["mode"] != options.mode:
            raise ValueError("P0 private input ownership/mode mismatch")
    else:
        shutil.copytree(options.template.resolve(), root, ignore=shutil.ignore_patterns("VT"))
    relative = Path("Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default")
    scene_path = root / relative / "SC_composited_vt_4x4.json"
    scene = read(scene_path)
    terrains = [item for item in scene["objects"] if item["type"] == "terrain"]
    selected = terrains[0]
    scene["objects"] = [item for item in scene["objects"] if item["type"] != "terrain"] + [selected]
    scene["description"] = "P0 真实 Pegasus tile00 生产负载：两种权重独立运行，非零 height blend，GPU timestamp 测量。"
    terrain_path = root / selected["terrainPath"]
    terrain = read(terrain_path)
    terrain["compositedVT"]["virtualResolution"] = [options.resolution, options.resolution]
    terrain["compositedVT"]["packagePath"] = "VT/P0_" + options.mode + "_" + str(options.resolution) + "/manifest.json"
    for field in ("materialInstancePath", "detailMaterialInstancePath"):
        material_path = root / terrain[field]
        material = read(material_path)
        material.setdefault("parameters", {}).pop("u_heightBlendStrength", None)
        material["parameters"].pop("u_terrainWeightMapMode", None)
        if options.mode == "local":
            material["parameters"]["u_terrainWeightMapMode"] = 1.0
        write(material_path, material)
    write(terrain_path, terrain)
    write(scene_path, scene)
    write(root / "p0-producer-input.json", {
        "scene": scene_path.relative_to(root).as_posix(), "mode": options.mode,
        "heightBlendStrength": 3, "virtualResolution": [options.resolution, options.resolution],
        "scope": "Actual tile00 source dimensions/catalog/frame; all virtual mips; Full16 baseline remains 8192.",
        "notClaimed": "All 16 terrain inputs, cross-block production or V2 runtime cache"})
    print(root)


if __name__ == "__main__":
    main()
