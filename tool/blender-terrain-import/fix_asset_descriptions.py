"""修复生成资产里被错误编码的中文描述。

背景：Blender 的 Python 在中文 Windows 上默认按 GBK 处理源码字符串，
导致 ``build_pegasus_library_global.py`` 写出的 manifest 描述在
``split_pegasus_tiles.py`` 透传时已经损坏。数据（高度图、层、权重）没有问题，
只有 ``description`` / ``sceneDescription`` 字段受影响，而
``SceneAssetValidator`` 要求描述非空且可读。

这个脚本把 SC 与全部 TR 的描述重写成干净的 UTF-8 文本。

用法：
    py -3 tool\\blender-terrain-import\\fix_asset_descriptions.py --resource-root <res>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

TR_DESCRIPTION = (
    "VL_HF 节点库生成的 4 km 地形切片：Heightfield → Noise（amp 500，"
    "element 按场景尺寸等比放大）→ Distort → Erode。参数取自 Pegasus 原工程的 "
    "heightfield_noise1 / distort1 / erode1，用于与 Houdini 基准比较统计量。")

SC_DESCRIPTION = (
    "Pegasus 4×4 地形验收场景：一张 4 km 全局高度场切成 16 个 1 km tile，"
    "共享边界采样、layer key、纹理数组与 32×32 VT 页表；"
    "由 VL_HF_PegasusTerrain 节点库生成，用于复刻飞马 PCG 地形，"
    "不参与论文数值验证。")


def _write(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=4) + "\n",
                    encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output-relative", default="Generated/BlenderPegasusGN")
    args = parser.parse_args()

    root = (args.resource_root / args.output_relative).resolve()
    touched = 0

    for tr_path in sorted(root.glob("tile_*/TR_*.json")):
        data = json.loads(tr_path.read_text(encoding="utf-8-sig"))
        data["description"] = TR_DESCRIPTION
        _write(tr_path, data)
        touched += 1

    for sc_path in sorted(root.glob("SC_*.json")):
        data = json.loads(sc_path.read_text(encoding="utf-8-sig"))
        data["description"] = SC_DESCRIPTION
        _write(sc_path, data)
        touched += 1

    # manifest 里的描述同样修一遍
    for manifest_path in sorted(root.glob("Global/*.json")):
        data = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        if "description" in data:
            data["description"] = TR_DESCRIPTION
        if "sceneDescription" in data:
            data["sceneDescription"] = SC_DESCRIPTION
        _write(manifest_path, data)
        touched += 1

    print(f"FIXED {touched} asset files under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
