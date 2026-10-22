"""Create synthetic TFX guides and a formal VL scene under resourceRoot/Generated.

This is geometry/visibility evidence, not an export of AMD's female asset.
The caller supplies the ordinary Common pass assets and selects this scene.
"""

import argparse
import json
import math
from pathlib import Path
import struct


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=4) + "\n", encoding="utf-8")


def generate(resource_root):
    relative = Path("Generated/Validation/SC_tressfx_static_probe")
    folder = resource_root / relative
    folder.mkdir(parents=True, exist_ok=False)
    source = folder / "Source"
    source.mkdir()
    guides, points = 96, 32
    header = bytearray(160)
    struct.pack_into("<fIIIIIII", header, 0, 4.0, guides, points, 160, 0, 0, 0, 0)
    positions = bytearray()
    for strand in range(guides):
        root_x = (strand / (guides - 1) - 0.5) * 68
        phase = strand * 0.17
        for point in range(points):
            t = point / (points - 1)
            x = root_x + 5 * t * math.sin(7 * t + phase)
            y = 38 - 76 * t
            z = 5 * math.sin(5 * t + phase) + 3 * math.cos(root_x / 18)
            positions += struct.pack("<ffff", x, y, z, 0 if point < 2 else 1)
    (source / "synthetic.tfx").write_bytes(header + positions)
    prefix = relative.as_posix()
    write_json(folder / "Meshes/SM_guides.json", {
        "name": "SM_synthetic_guides", "type": "tressfx",
        "modelDataPath": f"{prefix}/Source/synthetic.tfx", "strandRadius": 0.11,
        "materialSlots": [{"name": "hair", "materialInstancePath": f"{prefix}/Materials/MI_pink.json"}],
    })
    for name, tint in (("pink", None), ("cyan", [0.05, 0.65, 0.9, 1])):
        instance = {"name": f"Synthetic {name}", "type": "materialInstance",
                    "material": "shader/glsl/hair/M_strandProbe.json"}
        if tint is not None:
            instance["parameters"] = {"u_tintColor": tint}
        write_json(folder / f"Materials/MI_{name}.json", instance)
    objects = [
        {"name": "Front", "type": "mesh", "modelPath": f"{prefix}/Meshes/SM_guides.json",
         "position": [-44, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
        {"name": "Rotated", "type": "mesh", "modelPath": f"{prefix}/Meshes/SM_guides.json",
         "materialOverride": f"{prefix}/Materials/MI_cyan.json",
         "position": [44, 0, 0], "rotation": [0, 55, 0], "scale": [1, 1, 1]},
        {"name": "ProbeCamera", "type": "camera", "position": [0, 2, 190],
         "look_at": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1],
         "fov": 60, "near_clip": 1, "far_clip": 1000,
         "exposure": {"mode": "ev100", "ev100": 0, "compensation": 0,
                      "aperture": 4, "shutterSeconds": 0.008, "iso": 100}},
        {"name": "Black", "type": "environment", "environment": {"type": "none", "intensity": 0}},
    ]
    scene = f"{prefix}/SC_tressfx_static_probe.json"
    write_json(resource_root / scene, {
        "name": "TressFX Static Guide Probe", "type": "scene", "worldUnit": "cm",
        "description": "合成 guide 几何可见性探针：核对 TressFX5-5.4.4 顶点工厂的相机朝向 ribbon、"
                       "根梢拓扑及旋转实例；Unlit、夸大半径，不是女性资产、Marschner 或抗锯齿验收。",
        "objects": objects,
    })
    return {"scene": scene, "guideCount": guides, "pointsPerStrand": points,
            "ribbonIndices": guides * (points - 1) * 6}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    options = parser.parse_args()
    print(json.dumps(generate(options.resource_root.resolve()), indent=2))
