"""Create the article-composition VL scene from the UE level resources."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCENE = "Maps/SC_lookdev"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path)
    args = parser.parse_args()
    resource = args.resource_root or Path(
        json.loads((ROOT / "config/config.json").read_text(encoding="utf-8"))["resourcePath"]
    )
    resource = resource.resolve()
    article_hdri = resource / SCENE / "Source/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr"
    if not article_hdri.is_file():
        raise FileNotFoundError(
            f"UE article HDRI is missing; run convert_assets_preview.ps1 first: {article_hdri}"
        )

    scene = {
        "name": "Lookdev - UE Article Scene",
        "type": "scene",
        "worldUnit": "cm",
        "description": (
            "按统一资产审阅文章恢复的标准 lookdev 构图，直接使用 UE "
            "DataPrepView 关卡中的 SM_ColorCalibrator 资源及其关卡变换，"
            "使用 EpicQuadPanorama_Gray 单张 HDRI 作为环境照明和反射源。"
            "不替代 VulkanLearn 固定 EV100=0 的数值校准基线；不含 TAA/FSR。"
        ),
        "objects": [
            {
                "name": "UE_ColorCalibrator",
                "type": "mesh",
                "modelPath": f"{SCENE}/Meshes/SM_ue_color_calibrator.json",
                # UE is Z-up; VL scenes are Y-up. Preserve the UE instance
                # transform after converting its world basis into VL space.
                "position": [-105.0, 41.0, 35.0],
                "rotation": [-90.0, -119.9999, 0.0],
                "scale": [300.0, 300.0, 300.0],
            },
            {
                "name": "LookdevCamera",
                "type": "camera",
                "fov": 48,
                "near_clip": 1,
                "far_clip": 3000,
                "position": [-660.0, 300.0, -355.0],
                "look_at": [-105.0, 41.0, 35.0],
                "rotation": [0, 0, 0],
                "scale": [1, 1, 1],
                "exposure": {
                    "mode": "ev100",
                    "ev100": 0,
                    "aperture": 4,
                    "shutterSeconds": 0.008,
                    "iso": 100,
                    "compensation": 0,
                },
            },
            {
                "name": "NeutralStudio",
                "type": "environment",
                "environment": {
                    "type": "hdri",
                    "hdrPath": f"{SCENE}/Source/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr",
                    "cubeSize": 512,
                    "intensity": 1,
                },
            },
        ],
    }
    article_output = resource / "Generated/Lookdev/SC_lookdev_article.json"
    article_output.parent.mkdir(parents=True, exist_ok=True)
    article_output.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Authored {article_output}")


if __name__ == "__main__":
    main()
