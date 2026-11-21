"""Compare completed native lit/depth raw channels; never manufacture missing ID/UV/TES channels."""
import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from rvt_visual_oracle import digest, image_metrics, read, require, silhouette_metrics, thresholds

ROOT = Path(__file__).resolve().parents[3]


def decode_channel(path, record):
    width, height = record["width"], record["height"]
    require(width == height == 128, "Native lit reader requires the bounded 128-square target")
    data = path.read_bytes()
    require(len(data) == record["bytes"] == width*height*4, "Native channel byte layout mismatch")
    if record["channel"] == "deviceDepth":
        require(record["format"] == 126, "Only actual D32_SFLOAT depth is currently decoded")
        values = np.frombuffer(data, "<f4").reshape(height, width).copy()
        require(np.isfinite(values).all() and np.all((values >= 0) & (values <= 1)), "Invalid native depth")
        return values
    require(record["channel"] == "lit" and record["format"] in (37, 43, 44, 50),
            "Expected actual RGBA8/BGRA8 display bytes")
    values = np.frombuffer(data, np.uint8).reshape(height, width, 4)
    return values[:, :, [2, 1, 0] if record["format"] in (44, 50) else [0, 1, 2]].copy()


def compare(metadata_path, masks_path, native_build):
    metadata_path = Path(metadata_path)
    native = read(metadata_path)
    require(native["gpuExecuted"] and native["completedEpoch"] >= native["submissionEpoch"] > 0,
            "Native submission/completed-fence evidence missing")
    require(len(native["actualDraws"]) > 0 and len(native["cameraViewProjectionBlake3"]) == 64,
            "Actual native camera/draw identity missing")
    require(native["originalEvaluatorSha256"] ==
            "d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b", "Wrong original evaluator")
    build = read(native_build)
    executable = Path(build["executable"])
    require(digest(executable) == build["executableSha256"], "Native hash verifier changed")
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT / "build-vt-release/bin")+";"+environment.get("PATH", "")
    arrays, identities = {}, {}
    for channel in native["channels"]:
        key = (channel["role"], channel["channel"])
        require(key not in arrays and channel["submissionEpoch"] == native["submissionEpoch"] and
                channel["completedEpoch"] == native["completedEpoch"], "Duplicate/mismatched native epoch channel")
        path = metadata_path.parent / channel["path"]
        require(path.resolve().parent == metadata_path.resolve().parent, "Raw channel escaped capture directory")
        hashed = subprocess.run([str(executable), "--hash", str(path.resolve())],
            env=environment, check=True, capture_output=True, text=True).stdout.strip()
        require(hashed == channel["payloadBlake3"], "Native channel BLAKE3 mismatch")
        arrays[key] = decode_channel(path, channel)
        identities[str(key)] = {"sha256": digest(path), "blake3": hashed}
    require(set(arrays) == {(role, channel) for role in ("candidate", "reference")
                           for channel in ("lit", "deviceDepth")}, "Missing native lit/depth branch")
    masks = read(masks_path)
    require(masks["rect"] == native["rect"] and masks["cameraViewProjectionBlake3"] ==
            native["cameraViewProjectionBlake3"], "Independent fixed masks bound to a different camera/ROI")
    payload = Path(masks_path).parent / masks["path"]
    require(digest(payload) == masks["sha256"], "Fixed masks changed")
    with np.load(payload, allow_pickle=False) as data:
        roi, domain = data["terrainRoi"].copy(), data["silhouetteDomain"].copy()
    require(roi.shape == domain.shape == (128, 128) and roi.dtype == domain.dtype == np.bool_,
            "Independent fixed boolean ROI masks required")
    lit = image_metrics(arrays["candidate", "lit"], arrays["reference", "lit"], roi)
    require(masks["deviceDepthClear"] == 1.0, "Actual geometry clear-depth contract changed")
    candidate_valid = arrays["candidate", "deviceDepth"] < 1.0
    reference_valid = arrays["reference", "deviceDepth"] < 1.0
    silhouette = silhouette_metrics(candidate_valid, reference_valid, domain)
    common = candidate_valid & reference_valid & roi
    require(common.any(), "Empty common visible ROI")
    depth = np.abs(arrays["candidate", "deviceDepth"][common].astype(float) -
                   arrays["reference", "deviceDepth"][common].astype(float))
    limits, frozen = thresholds()
    return {"scope": "Two native same-epoch replay branches only; no substitution for actual-frame output calibration",
            "nativeMetadataSha256": digest(metadata_path), "fixedMasksSha256": digest(masks_path),
            "payloads": identities, "frame": native["frame"], "submissionEpoch": native["submissionEpoch"],
            "lit": lit, "silhouette": silhouette, "silhouetteMeaning": "All replayed opaque depth geometry; no invented terrain ID",
            "depth": {"maxAbs": float(depth.max()), "meanAbs": float(depth.mean())},
            "frozenSha256": frozen,
            "diagnosticThresholds": {"mae": lit["maeBytes"] <= limits["litTerrainRoiMaxMaeBytes"],
                "rmse": lit["rmseBytes"] <= limits["litTerrainRoiMaxRmseBytes"],
                "p99": lit["p99ErrorBytes"] <= limits["litTerrainRoiMaxP99ErrorBytes"],
                "silhouette": silhouette["xorFraction"] <= limits["maxSilhouetteXorFraction"]},
            "missingAcceptanceFacts": ["Replay candidate vs actual frame output calibration",
                "Pixel surface ID/patch LOD/local UV/global UV/world position",
                "Independent verification of actual sampled page-slot footprint",
                "Separate actual TES R16 numeric gate and complete pose/mode/dynamic matrix"],
            "overallP5Passed": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-metadata", type=Path, required=True)
    parser.add_argument("--fixed-masks", type=Path, required=True)
    parser.add_argument("--native-hash-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), "Preserve existing comparison")
    report = compare(args.native_metadata, args.fixed_masks, args.native_hash_build)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps({"output": str(args.output), "overallP5Passed": False}))


if __name__ == "__main__":
    main()
