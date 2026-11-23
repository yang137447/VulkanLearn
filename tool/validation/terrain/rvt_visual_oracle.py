"""Strict bounded visual/TES evidence reader. Never renders or substitutes quality counts."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import numpy as np
from height_bilinear_reference import bilinear, validate_oracle_anchors

ROOT = Path(__file__).resolve().parents[3]
MAX_ARRAY_BYTES = 64 * 1024 * 1024
TES_HEADER_BYTES = 64
TES_STRIDE_FLOATS = 48


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def thresholds():
    frozen = ROOT / "config/terrainRvtP0Frozen.json"
    document = read(frozen)
    return document["qualityTargets"], digest(frozen)


def image_metrics(candidate, reference, roi):
    require(candidate.shape == reference.shape and candidate.ndim == 3 and candidate.shape[2] == 3,
            "Lit inputs must have matching HxWx3 shape")
    require(candidate.dtype == reference.dtype == np.uint8, "Lit gate uses display byte RGB, no implicit normalization")
    require(roi.shape == candidate.shape[:2] and roi.dtype == np.bool_, "Explicit boolean terrain ROI required")
    require(np.any(roi), "Empty terrain ROI")
    delta = np.abs(candidate[roi].astype(np.float64) - reference[roi].astype(np.float64))
    return {"pixels": int(roi.sum()), "channelSamples": int(delta.size),
            "maeBytes": float(delta.mean()), "rmseBytes": float(np.sqrt(np.mean(delta * delta))),
            "p99ErrorBytes": float(np.quantile(delta, .99, method="linear")),
            "maxChannelErrorBytes": float(delta.max()),
            "definition": "Equal-weight RGB channel errors on the fixed independently supplied terrain ROI; no registration, exposure fit, blur, erosion or mask from candidate errors"}


def silhouette_metrics(candidate, reference, roi):
    require(candidate.dtype == reference.dtype == roi.dtype == np.bool_, "Silhouette masks must be boolean")
    require(candidate.shape == reference.shape == roi.shape and np.any(roi), "Invalid fixed silhouette domain")
    xor = (candidate != reference) & roi
    return {"xorPixels": int(xor.sum()), "domainPixels": int(roi.sum()),
            "xorFraction": float(xor.sum() / roi.sum()),
            "definition": "XOR on a separately fixed viewport/ROI domain, including uncovered pixels; never intersection-only"}


def load_packet(path, role):
    path = Path(path).resolve()
    metadata = read(path)
    require(metadata["version"] == 1 and metadata["role"] == role, "Wrong packet version/role")
    payload = (path.parent / metadata["payload"]["path"]).resolve()
    require(payload.stat().st_size <= MAX_ARRAY_BYTES, "Packet file exceeds bounded reader cap")
    require(digest(payload) == metadata["payload"]["sha256"], "Packet payload hash mismatch")
    require(metadata["binding"]["samplingDomain"] == "visible-geometry-roi", "Feedback domain is not visual geometry")
    with np.load(payload, allow_pickle=False) as archive:
        arrays = {name: archive[name] for name in archive.files}
    require(sum(value.nbytes for value in arrays.values()) <= MAX_ARRAY_BYTES, "Decoded packet exceeds CPU cap")
    for value in arrays.values():
        require(value.dtype.kind in "buif" and np.isfinite(value).all(), "Nonfinite/non-numeric packet")
    for name in ("litRgb", "deviceDepth", "worldPosition", "localUv", "globalUv",
                 "surfaceId", "lod", "topology", "geometryValid", "terrainRoi", "silhouetteDomain"):
        require(name in arrays, "Missing actual capture channel: " + name)
    height, width = arrays["litRgb"].shape[:2]
    require(0 < width <= 512 and 0 < height <= 512, "Capture must be a bounded <=512x512 ROI")
    for name, channels in (("worldPosition", 3), ("localUv", 2), ("globalUv", 2)):
        require(arrays[name].shape == (height, width, channels), "Channel shape mismatch: " + name)
    for name in ("deviceDepth", "surfaceId", "lod", "topology", "geometryValid", "terrainRoi", "silhouetteDomain"):
        require(arrays[name].shape == (height, width), "Scalar channel shape mismatch: " + name)
    for name in ("deviceDepth", "worldPosition", "localUv", "globalUv"):
        require(arrays[name].dtype == np.float32, "Raw float32 channel required: " + name)
    for name in ("surfaceId", "lod", "topology"):
        require(arrays[name].dtype == np.uint32, "Raw uint32 identity channel required: " + name)
    for name in ("geometryValid", "terrainRoi", "silhouetteDomain"):
        require(arrays[name].dtype == np.bool_, "Boolean mask required: " + name)
    require(metadata["binding"]["captureRect"][2:] == [width, height], "Capture rectangle mismatch")
    require(metadata["nativeCompletion"]["completedEpoch"] >= metadata["binding"]["submissionEpoch"],
            "Capture has not completed at its submission fence")
    require(metadata["nativeCompletion"]["complete"] and not metadata["nativeCompletion"]["overflow"],
            "Incomplete/overflowed capture")
    require(metadata["provenance"]["gpuExecuted"] and not metadata["provenance"].get("synthetic", False),
            "Synthetic or unexecuted packet cannot be accepted")
    return metadata, arrays


def compare_packets(candidate_path, reference_path):
    candidate_meta, candidate = load_packet(candidate_path, "candidate")
    reference_meta, reference = load_packet(reference_path, "reference")
    require(candidate_meta["binding"] == reference_meta["binding"],
            "Camera/epoch/World/pose/render/input identity mismatch")
    require(candidate_meta["provenance"]["authority"] == "native-draw-and-lit-output",
            "Candidate must come from actual draws and lit output")
    require(reference_meta["provenance"]["authority"] == "independent-current-evaluator",
            "Independent pinned current reference required")
    require(reference_meta["provenance"]["referenceCalibrationPassed"], "Reference calibration missing/failed")
    require(candidate_meta["provenance"]["heightShaderStage"] == "tessellation-evaluation",
            "Compute-only height sampling is not actual TES")
    require(candidate_meta["provenance"]["drawMetadataFromSnapshot"], "Actual draw metadata required")
    require(np.array_equal(candidate["terrainRoi"], reference["terrainRoi"]), "ROI must be locked independently")
    require(np.array_equal(candidate["silhouetteDomain"], reference["silhouetteDomain"]), "Silhouette domain differs")
    limits, frozen_hash = thresholds()
    lit = image_metrics(candidate["litRgb"], reference["litRgb"], reference["terrainRoi"])
    silhouette = silhouette_metrics(candidate["geometryValid"], reference["geometryValid"], reference["silhouetteDomain"])
    common = candidate["geometryValid"] & reference["geometryValid"] & reference["terrainRoi"]
    require(np.any(common), "No common visible terrain geometry")
    differences = {}
    for name in ("deviceDepth", "worldPosition", "localUv", "globalUv"):
        delta = np.abs(candidate[name][common].astype(float) - reference[name][common].astype(float))
        differences[name] = {"maxAbs": float(delta.max()), "meanAbs": float(delta.mean())}
    differences["surfaceIdMismatches"] = int(np.count_nonzero(candidate["surfaceId"][common] != reference["surfaceId"][common]))
    differences["lodMismatches"] = int(np.count_nonzero(candidate["lod"][common] != reference["lod"][common]))
    differences["topologyMismatches"] = int(np.count_nonzero(candidate["topology"][common] != reference["topology"][common]))
    gates = {"litMae": lit["maeBytes"] <= limits["litTerrainRoiMaxMaeBytes"],
             "litRmse": lit["rmseBytes"] <= limits["litTerrainRoiMaxRmseBytes"],
             "litP99": lit["p99ErrorBytes"] <= limits["litTerrainRoiMaxP99ErrorBytes"],
             "silhouette": silhouette["xorFraction"] <= limits["maxSilhouetteXorFraction"],
             "surfaceIdentity": differences["surfaceIdMismatches"] == 0,
             "drawLod": differences["lodMismatches"] == 0,
             "drawTopology": differences["topologyMismatches"] == 0}
    return {"scope": "One bounded same-binding ROI only; TES numeric, whole matrix and dynamics are separate gates",
            "binding": candidate_meta["binding"], "frozenSha256": frozen_hash, "lit": lit,
            "silhouette": silhouette, "geometryDiagnostics": differences, "gates": gates,
            "boundedImageGatesPassed": all(gates.values()), "overallP5Passed": False}


def decode_tes(path):
    data = Path(path).read_bytes()
    require(len(data) >= TES_HEADER_BYTES and (len(data) - TES_HEADER_BYTES) % (TES_STRIDE_FLOATS * 4) == 0,
            "Invalid TES capture allocation length")
    header = np.frombuffer(data[:TES_HEADER_BYTES], "<u4").reshape(4, 4).copy()
    count, overflow, first_primitive, primitive_count = map(int, header[0])
    capacity = (len(data) - TES_HEADER_BYTES) // (TES_STRIDE_FLOATS * 4)
    require(overflow == 0 and 0 < count <= capacity, "Empty/overflowed TES capture")
    records = np.frombuffer(data, "<f4", count * TES_STRIDE_FLOATS, TES_HEADER_BYTES).reshape(count, 12, 4).copy()
    require(np.isfinite(records).all(), "Nonfinite TES records")
    return header, records


def source_mip2(manifest_path, surface_id):
    manifest_path = Path(manifest_path)
    publication = read(manifest_path)["production"]
    surface = next(item for item in publication["surfaces"] if item["surfaceId"] == surface_id)
    binding = surface["inputs"]
    object_id = binding[8] if isinstance(binding, list) else binding["layerHeight"]
    obj = next(item for item in publication["objects"] if item["objectId"] == object_id)
    desc = obj["image"]
    require(desc["format"] == "r16-unorm" and desc["mipLevels"] >= 3, "Actual frozen R16 source mip2 required")
    payload = manifest_path.parent / obj["path"]
    values = np.frombuffer(payload.read_bytes(), "<u2")
    stride = sum(max(1, desc["width"] >> level) * max(1, desc["height"] >> level)
                 for level in range(desc["mipLevels"]))
    require(len(values) == stride * desc["layers"], "Frozen layer-major mip-major image byte count mismatch")
    offset = desc["width"] * desc["height"] + max(1, desc["width"] >> 1) * max(1, desc["height"] >> 1)
    width, height = max(1, desc["width"] >> 2), max(1, desc["height"] >> 2)
    planes = [values[layer * stride + offset:layer * stride + offset + width * height] for layer in range(desc["layers"])]
    return surface, planes, width, height, {"path": str(payload), "sha256": digest(payload)}


def compare_tes(capture_path, metadata_path, manifest_path):
    metadata = read(metadata_path)
    header, records = decode_tes(capture_path)
    require(metadata["stage"] == "tessellation-evaluation" and metadata["gpuExecuted"], "Actual TES execution required")
    require(not metadata.get("synthetic", False), "Synthetic fixture is not TES evidence")
    require(metadata["primitiveTopology"] == "patch-list" and metadata["actualLod"] == 0, "Actual active LOD0 patch draw required")
    require(metadata["captureSha256"] == digest(capture_path), "TES binary hash mismatch")
    require(metadata["drawWords"][2:] == [0, 10], "TES header must identify actual LOD0/VK_PATCH_LIST")
    require(metadata["completedEpoch"] >= metadata["submissionEpoch"], "TES capture not fence-complete")
    require(metadata["baseFineResidentPages"] == 0, "Independent height closure requires zero base fine residency witness")
    require(list(map(int, header[1])) == metadata["frameEpochWords"], "TES header frame/epoch mismatch")
    require(list(map(int, header[2])) == metadata["worldPoseWords"], "TES header World/pose mismatch")
    require(list(map(int, header[3])) == metadata["drawWords"], "TES draw identity mismatch")
    surface, planes, width, height, identity = source_mip2(manifest_path, metadata["surfaceId"])
    require(metadata["publicationSha256"] == digest(manifest_path), "TES publication identity mismatch")
    validate_oracle_anchors()
    maximum = 0.0
    active_samples = 0
    predicted_world = []
    for record in records:
        weights = record[6].astype(float)
        heights = record[7].astype(float)
        ids = record[5].astype(int)
        expected = np.repeat(.5, 4)
        for slot in range(4):
            if weights[slot] <= 0:
                continue
            layer = int(ids[slot])
            require(0 <= layer < len(surface["globalToSourceSlice"]), "Invalid captured global layer")
            source_slice = surface["globalToSourceSlice"][layer]
            require(0 <= source_slice < len(planes), "Unmapped captured source slice")
            uv = [float(record[8, slot]), float(record[9, slot])]
            expected[slot] = bilinear(planes[source_slice], width, height, uv)["exactR16"] / 65535
            maximum = max(maximum, abs(heights[slot] - expected[slot]) * 65535)
            active_samples += 1
        scalar_cm = float(np.sum(weights * (expected - .5) * record[10]) * 100 * record[3, 3] * record[1, 3])
        predicted_world.append(record[1, :3].astype(float) + record[3, :3].astype(float) * scalar_cm)
    require(active_samples > 0 and np.any(records[:, 1, 3] > 0) and np.any(records[:, 3, 3] != 0),
            "No active height/displacement evidence")
    active = ((records[:, 1, 3] > 0) & (records[:, 3, 3] != 0) &
              (records[:, 2, 3] > 0) & (records[:, 2, 3] < records[:, 11, 2]))
    require(np.any(active), "No captured invocation inside active TES radius")
    require(np.any(np.linalg.norm(records[:, 2, :3] - records[:, 1, :3], axis=1)[active] > 0),
            "No actual nonzero TES displacement witness")
    limits, frozen_hash = thresholds()
    return {"scope": "Bounded actual TES records vs independent double source-mip2 oracle; not scene-lit or all-edge acceptance",
            "records": len(records), "heightSamples": active_samples, "sourceMip2": identity,
            "maxHeightErrorR16Units": float(maximum), "gateR16Units": float(limits["tesMaxHeightErrorR16Units"]),
            "heightGatePassed": bool(maximum <= limits["tesMaxHeightErrorR16Units"]),
            "predictedWorldMaxAbsCm": float(np.max(np.abs(np.asarray(predicted_world) - records[:, 2, :3]))),
            "worldDiagnosticScope": "Float64 reconstruction reported without inventing a world-coordinate gate",
            "frozenSha256": frozen_hash, "overallP5Passed": False}


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    image = commands.add_parser("images")
    image.add_argument("--candidate", type=Path, required=True)
    image.add_argument("--reference", type=Path, required=True)
    tes = commands.add_parser("tes")
    tes.add_argument("--capture", type=Path, required=True)
    tes.add_argument("--metadata", type=Path, required=True)
    tes.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        result = compare_packets(options.candidate, options.reference) if options.command == "images" else compare_tes(
            options.capture, options.metadata, options.manifest)
        passed = result.get("boundedImageGatesPassed", result.get("heightGatePassed", False))
    except (ValueError, KeyError, OSError, StopIteration) as error:
        result, passed = {"status": "missing-or-invalid-evidence", "error": str(error), "overallP5Passed": False}, False
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if passed else 2)


if __name__ == "__main__":
    main()
