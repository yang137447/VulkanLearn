from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


ASSET_PATH = "/Game/TemplateLevels/Assets/HDRI/EpicQuadPanorama_Gray"


def main() -> None:
    asset = unreal.load_asset(ASSET_PATH)
    result = {
        "assetPath": ASSET_PATH,
        "loaded": asset is not None,
        "class": asset.get_class().get_name() if asset else None,
        "unrealExporters": sorted(
            name for name in dir(unreal)
            if "exporter" in name.lower() or "textureexport" in name.lower()
        ),
    }
    if asset:
        result["properties"] = sorted(
            name for name in dir(asset)
            if "source" in name.lower() or "pixel" in name.lower() or "texture" in name.lower()
        )
    output = Path(os.environ["VL_UE_PROBE_OUTPUT"])
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    unreal.log("[VL Lookdev Probe] " + json.dumps(result, ensure_ascii=False))
    unreal.SystemLibrary.quit_editor()


main()
