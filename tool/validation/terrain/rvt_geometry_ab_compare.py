"""Independent saved-BMP comparison; preserves all source pixels."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    images, records = [], []
    for label, stage_name in (("before", "gb1"), ("after", "ga1")):
        folder = output / label
        stage = ROOT.parent / "p5g" / stage_name
        screenshot = folder / "junction_0_0.bmp"
        data = screenshot.read_bytes()
        length, offset = struct.unpack_from("<I", data, 2)[0], struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        bits = struct.unpack_from("<H", data, 28)[0]
        expected = offset + ((width * bits + 31) // 32 * 4) * abs(height)
        assert data[:2] == b"BM" and len(data) == length == expected
        with Image.open(screenshot) as image:
            images.append(np.asarray(image.convert("RGB")).copy())
        result = read(folder / "result.json")
        quality = read(folder / "capture-await-quality.json")
        state = quality["qualityState"]
        assert state["poseId"] == 2 and state["validAtRawTargetPlusOne"] / state["visible"] >= .99
        assert state["incorrectIdentity"] == 0 and state["unresolved"] == 0
        assert result["exitCode"] == 0 and not result["errors"]
        text = (folder / "runtime.log").read_text(encoding="utf-8", errors="replace")
        targets = ("Landscape_00|3|3", "Landscape_01|0|3", "Landscape_04|3|0", "Landscape_05|0|0")
        lod = [line for line in text.splitlines() if "[TerrainLOD]" in line and any(target in line for target in targets)]
        assert len(lod) >= 4 and all("level=0/3" in line for line in lod)
        config = read(stage / "config/config.json")
        scene_path = stage / "resources" / config["initScene"]
        scene = read(scene_path)
        schema_path = stage / "shader/glsl/M_terrain.json"
        schema = read(schema_path)
        parameters = {name: value["default"] for name, value in schema["parameters"].items()}
        materials = []
        bindings = {str(path.relative_to(stage)): digest(path)
                    for path in (stage / "main_game.exe", stage / "config/config.json",
                                 stage / "config/renderGraphConfig.json", scene_path, schema_path,
                                 stage / "resources/Common/Materials/Pass/MI_toneMapping.json")}
        for placement in scene["objects"]:
            if placement.get("name") not in ("Landscape_00", "Landscape_01", "Landscape_04", "Landscape_05"):
                continue
            terrain_path = stage / "resources" / placement["terrainPath"]
            terrain = read(terrain_path)
            bindings[str(terrain_path.relative_to(stage))] = digest(terrain_path)
            for field in ("materialInstancePath", "detailMaterialInstancePath"):
                path = stage / "resources" / terrain[field]
                material = read(path)
                bindings[str(path.relative_to(stage))] = digest(path)
                effective = parameters | material.get("parameters", {})
                if field == "detailMaterialInstancePath":
                    assert material["macros"]["VL_TERRAIN_TESSELLATION"] == 1
                    assert effective["u_layerDisplacementScale"] == 1
                materials.append({"placement": placement["name"], "field": field,
                    "path": terrain[field], "macros": material.get("macros", {}),
                    "effectiveSerializedParameters": effective})
        manifest = read(stage / "resources/VT/775359b06fde29e5/manifest.json")
        records.append({"label": label, "stage": str(stage), "bmpSha256": digest(screenshot),
            "bmp": {"actualLength": len(data), "declaredLength": length, "pixelOffset": offset,
                    "width": width, "height": height, "bitsPerPixel": bits},
            "quality": quality, "cameraAck": result["camera"],
            "targetChunkLod": lod, "staticInputBindings": bindings,
            "materialParameterScope": "Effective serialized schema+runtime MI values; not a runtime UBO readback. Service frozen static bindings are recorded separately.",
            "materials": materials,
            "frozenProduction": {"hash": manifest["production"]["hash"],
                "surfaces": [{"surfaceId": surface["surfaceId"], "parameters": surface["parameters"],
                              "sampling": surface["sampling"], "inputs": surface["inputs"]}
                             for surface in manifest["production"]["surfaces"]
                             if surface["surfaceId"].split("|")[-1] in ("Landscape_00", "Landscape_01", "Landscape_04", "Landscape_05")]},
            "environment": [item for item in scene["objects"] if item.get("type") == "environment"],
            "shaderSourceSha256": digest(stage / "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl"),
            "cacheSha256": digest(stage / "shader/spv/shader-build-cache.json"),
            "cacheLog": result["shaderCacheLog"],
            "validationDiagnostics": [line for line in text.splitlines()
                                      if re.search(r"VUID-|Validation (Error|Warning)|validation.*(error|warning)", line, re.I)],
            "harnessIssue": result.get("harnessIssue")})
    assert records[0]["staticInputBindings"] == records[1]["staticInputBindings"]
    assert records[0]["materials"] == records[1]["materials"]
    assert records[0]["environment"] == records[1]["environment"]
    assert records[0]["frozenProduction"] == records[1]["frozenProduction"]
    assert records[0]["cameraAck"] == records[1]["cameraAck"]
    regions = {}
    for name, bounds in (("seamStrip", (0, 340, 1280, 380)), ("central", (320, 320, 960, 400)),
                         ("full", (0, 0, 1280, 720))):
        left, top, right, bottom = bounds
        before, after = [image[top:bottom, left:right].astype(float) for image in images]
        delta = np.abs(before - after)
        regions[name] = {"boundsExclusive": bounds,
            "darkMaxRgbBelow80": [int(np.count_nonzero(np.max(image, axis=2) < 80)) for image in (before, after)],
            "darkMaxRgbBelow100": [int(np.count_nonzero(np.max(image, axis=2) < 100)) for image in (before, after)],
            "changedPixels": int(np.count_nonzero(np.max(delta, axis=2))),
            "meanAbsRgbByteDifference": float(delta.mean()),
            "maxAbsRgbByteDifference": float(delta.max())}
    combined = np.concatenate(images, axis=1)
    Image.fromarray(combined).save(output / "side-by-side.png")
    with Image.open(output / "side-by-side.png") as image:
        assert np.array_equal(np.asarray(image), combined)
    report = {"scope": "Native RGB effect, same static view after settled, no exact screenshot/quality epoch claim; no depth/worldpos/ID readback or full-island acceptance",
              "beforeLeftAfterRight": True, "sourcePixelsUnmodified": True, "regions": regions,
              "shots": records, "formalAcceptancePassed": False}
    (output / "independent-comparison.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(regions, indent=2))


if __name__ == "__main__":
    main()
