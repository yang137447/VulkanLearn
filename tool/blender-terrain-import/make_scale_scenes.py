"""生成不同 tile 数量的场景，用于定位地形数量与帧率的关系。

只改 terrain 对象数量，保持其它对象（灯光/相机/环境）不变。

用法：
    py -3 tool\\blender-terrain-import\\make_scale_scenes.py --resource-root <res>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output-relative", default="Generated/BlenderPegasusGN")
    args = parser.parse_args()

    root = (args.resource_root / args.output_relative).resolve()
    source = root / "SC_terrain_pegasus_4x4.json"
    scene = json.loads(source.read_text(encoding="utf-8-sig"))

    terrains = [o for o in scene["objects"] if o.get("type") == "terrain"]
    others = [o for o in scene["objects"] if o.get("type") != "terrain"]

    for count in (1, 4, 9, 16):
        subset = json.loads(json.dumps(scene))
        subset["objects"] = terrains[:count] + others
        subset["name"] = f"SC_terrain_scale_{count:02d}"
        subset["description"] = (
            f"性能对照场景：只放置 {count} 个 terrain 对象，"
            "其余灯光/相机/环境与 4x4 场景一致，用于定位帧率与地形数量的关系。")
        path = root / f"SC_terrain_scale_{count:02d}.json"
        path.write_text(json.dumps(subset, ensure_ascii=False, indent=4) + "\n",
                        encoding="utf-8")
        print(f"WROTE {path.name}: terrains={count}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
