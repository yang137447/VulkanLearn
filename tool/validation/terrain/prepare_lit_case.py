"""Author a `native-terrain-lit-replay-v1` case from real captured evidence.

CPU only. Every value written here comes from a real file: pinned BLAKE3 input
hashes, the r11 expanded ROI fragments, a TVQ1 quality capture at the pose, the
C010r14 publication manifest and the p3 original-evaluator page set.

Two artefacts are produced:

* `lit-case-plan.json` - always. The resolved pose, capture, ROI->feedback
  mapping, footprint, page availability and every hash. This is what
  `make_lit_masks.py` and `prepare_reference_pages.py` consume.
* `native-terrain-lit-replay-v1.json` - only when every footprint slot has a
  real original-evaluator page (or `--allow-incomplete-pages` is given, which
  records the gap inside the case and is NOT runnable evidence).
"""

import argparse
import datetime
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_lit_evidence as T
import prepare_reference_pages as P

POSE_EVIDENCE = ("C010r8-remaining24-case04: four real shots captured poseId 2,3,4,5; a single "
                 "`camera pose` command after the first quality Begin therefore yields poseId 2")


def verify_fragments(fragment_dir):
    prepared = T.read(Path(fragment_dir) / "prepared.json")
    report = {"prepared": str((Path(fragment_dir) / "prepared.json").resolve()),
              "preparedSha256": T.sha256(Path(fragment_dir) / "prepared.json"),
              "fragments": {}, "sourceInputs": {"checked": 0, "matched": 0, "mismatched": []}}
    for name in ("deferredLighting.frag", "toneMapping.frag"):
        path = Path(fragment_dir) / name
        text = path.read_text(encoding="utf-8", errors="replace")
        report["fragments"][name] = {"path": str(path.resolve()), "sha256": T.sha256(path),
                                     "bytes": path.stat().st_size,
                                     "bakesRect": ("576" in text and "296" in text and "128" in text)}
        if not report["fragments"][name]["bakesRect"]:
            raise ValueError("Expanded ROI fragment no longer bakes the fixed rect: " + name)
    inputs = prepared.get("shaderSourceInputs", {})
    for recorded, expected in sorted(inputs.items()):
        marker = "shader/glsl/"
        index = recorded.replace("\\", "/").find(marker)
        if index < 0:
            continue
        relative = recorded.replace("\\", "/")[index:]
        current = T.ROOT / relative
        if not current.exists():
            continue
        report["sourceInputs"]["checked"] += 1
        if T.sha256(current) != expected:
            report["sourceInputs"]["mismatched"].append({"path": relative,
                                                         "recorded": expected,
                                                         "current": T.sha256(current)})
        else:
            report["sourceInputs"]["matched"] += 1
    if report["sourceInputs"]["mismatched"]:
        raise ValueError("Expanded ROI fragments were generated from different shader sources")
    report["rect"] = prepared["rect"]
    report["fullViewport"] = prepared["fullViewport"]
    report["gpuExecuted"] = prepared.get("gpuExecuted", False)
    return report, prepared


def resolve_pose_id(pose_name, explicit, log_path):
    if explicit:
        return int(explicit), "explicit --pose-id"
    if log_path:
        pose_ids = []
        for line in Path(log_path).read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if isinstance(event, dict):
                payload = event.get("payload", event)
                if isinstance(payload, dict) and isinstance(payload.get("poseId"), (int, float)):
                    pose_ids.append(int(payload["poseId"]))
        if not pose_ids:
            raise ValueError("No poseId found in the runtime evidence log")
        return max(pose_ids), "runtime evidence log " + str(Path(log_path).resolve())
    return 2, POSE_EVIDENCE


def build_case(name, pose_name, mapping, capture_override, pose_id, pose_id_source,
               asset_root, manifest, delivery, page_source, fragment_dir, extra_reports=None,
               slot_capacity=None, allow_fine_resident=False):
    position, look_at, plan_path = T.resolve_pose(pose_name)
    direction = T._normalize([T._f32(look_at[i] - position[i]) for i in range(3)])
    if capture_override:
        metadata_path = Path(capture_override)
        binary_path = metadata_path.with_suffix(".bin")
        capture = T.QualityCapture(binary_path, metadata_path)
    else:
        metadata_path, binary_path, capture = T.select_capture(
            [T._f32(value) for value in position], direction)
    camera = [float(value) for value in capture.metadata["cameraPosition"]]
    drift = max(abs(camera[axis] - T._f32(position[axis])) for axis in range(3))
    if drift > 0.02:
        raise ValueError("Selected quality capture is not at the requested pose (%.4f)" % drift)
    observed_direction = T.capture_direction(capture.metadata)
    direction_drift = max(abs(observed_direction[axis] - direction[axis]) for axis in range(3))
    if capture.render_extent() != tuple(T.FULL_VIEWPORT):
        raise ValueError("Quality capture render extent differs from the baked ROI viewport")
    if mapping == "owners":
        # 近场帧：footprint = 该帧页表里 ROI 可见 surface 的全部常驻页（含根），
        # 这样 ROI 的每个 tap（含粗 mip 回退）都能在参考图集里找到正确字节。
        visible = T.roi_visible_surfaces(capture)
        footprint = T.footprint_from_owners(capture, visible)
        texels = T.roi_feedback_texels("roi-center", capture.render_extent())
        mapping_note_extra = (" owners: every live pool page of the ROI-visible surfaces (%d surfaces)"
                              % len(visible))
    else:
        footprint, texels = T.footprint_from_capture(capture, mapping)
        mapping_note_extra = ""
    if not footprint:
        raise ValueError("The ROI maps to no terrain claim in this capture")

    publication = T.read(manifest)
    surfaces = publication["production"]["surfaces"]
    stored = publication["surfaces"]
    if len(stored) != len(surfaces):
        raise ValueError("Publication surface dictionaries differ in length")
    channels = {tuple(surface["channels"]) for surface in stored}
    if channels != {tuple(T.ATLAS_CHANNELS)}:
        raise ValueError("Publication surface channels differ from the expected three RGBA8 planes")
    known_surfaces = {surface["surfaceId"] for surface in surfaces}
    for identity in footprint.values():
        if identity["surfaceId"] not in known_surfaces:
            raise ValueError("Footprint slot names an unpublished surface")
    if not 0 < len(footprint) <= T.ACTUAL_SLOT_CAPACITY:
        raise ValueError("Footprint exceeds the frozen physical slot capacity")
    if max(footprint) >= T.ACTUAL_SLOT_CAPACITY:
        raise ValueError("Observed slot index exceeds the frozen physical capacity")
    capacity = int(slot_capacity) if slot_capacity else T.ACTUAL_SLOT_CAPACITY
    if not max(footprint) < capacity <= T.ACTUAL_SLOT_CAPACITY:
        raise ValueError("Reference layout capacity must cover every observed slot (max %d)" % max(footprint))

    case_directory = Path(delivery) / ("case-" + name)
    case_directory.mkdir(parents=True, exist_ok=True)
    blake = T.Blake3()
    pages = P.prepare(footprint, page_source, case_directory / "pages", manifest, blake,
                      extra_reports=extra_reports)

    fragments, prepared = verify_fragments(fragment_dir)
    run_directory = Path(delivery) / "runs" / (name + "-r1") / "raw"
    output_relative = str(run_directory.resolve())

    pinned = []

    def pin(path, role):
        path = Path(path)
        if not path.exists():
            raise ValueError("Pinned input missing: " + str(path))
        pinned.append({"path": str(path.resolve()), "blake3": blake.hash(path), "role": role})

    pin(manifest, "publication-manifest")
    pin(Path(asset_root) / "config" / "config.json", "package-config")
    pin(Path(asset_root) / "config" / "renderGraphConfig.json", "package-render-graph")
    scene_document = T.read(Path(asset_root) / "config" / "config.json")
    scene = scene_document["initScene"]
    scene_path = Path(asset_root) / "resources" / scene
    pin(scene_path, "scene")
    for placement in T.read(scene_path)["objects"]:
        if placement.get("type") == "terrain":
            pin(Path(asset_root) / "resources" / placement["terrainPath"], "terrain-descriptor")
    for surface in surfaces:
        pin(Path(manifest).parent / surface["runtimeMaterialInstancePath"],
            "surface-runtime-material-instance")
        if surface.get("runtimeDetailMaterialInstancePath"):
            pin(Path(manifest).parent / surface["runtimeDetailMaterialInstancePath"],
                "surface-runtime-detail-material-instance")
    pin(T.ROOT / "config/terrainRvtP0Frozen.json", "frozen-p0")
    pin(Path(fragment_dir) / "deferredLighting.frag", "expanded-lighting-fragment")
    pin(Path(fragment_dir) / "toneMapping.frag", "expanded-tone-mapping-fragment")
    pin(Path(fragment_dir) / "prepared.json", "expanded-fragment-prepared-identity")
    pin(T.ROOT / "shader/glsl/pass/deferredLighting.frag", "source-lighting-fragment")
    pin(T.ROOT / "shader/glsl/pass/toneMapping.frag", "source-tone-mapping-fragment")
    pin(binary_path, "quality-capture")
    pin(metadata_path, "quality-capture-metadata")
    pin(Path(pages["sourceReport"]), "original-page-producer-report")
    pin(P.resolve_page_source_manifest(page_source), "original-page-source-manifest")
    pin(Path(__file__), "prepare-lit-case-tool")
    pin(Path(T.__file__), "lit-evidence-library")
    pin(Path(P.__file__), "prepare-reference-pages-tool")
    for entry in pages["entries"]:
        pin(entry["path"], "original-gpu-page")

    identity = {"pose": pose_name, "poseId": pose_id, "rect": T.RECT,
                "cameraPosition": camera, "footprint": sorted(footprint),
                "originalPages": [entry["blake3"] for entry in pages["entries"]],
                "publicationSha256": T.sha256(manifest)}
    input_closure = T.digest_document({"inputs": [[item["path"], item["blake3"]] for item in pinned],
                                       "identity": identity})
    reference_set = pages["referencePageSetSha256"]

    case = {
        "kind": "native-terrain-lit-replay-v1",
        "poseId": int(pose_id),
        "cameraPosition": camera,
        "rect": list(T.RECT),
        "expandedLightingFragment": str((Path(fragment_dir) / "deferredLighting.frag").resolve()),
        "expandedToneMappingFragment": str((Path(fragment_dir) / "toneMapping.frag").resolve()),
        "output": output_relative,
        "inputClosureSha256": input_closure,
        "originalEvaluatorSha256": T.ORIGINAL_EVALUATOR_SHA256,
        "referencePageSetSha256": reference_set,
        "actualRoiSlotFootprint": sorted(int(slot) for slot in footprint),
        "originalGpuPages": pages["entries"],
        "actualSlotCapacity": capacity,
        "actualAtlasFormats": [T.VK_FORMATS[name] for name in T.ATLAS_CHANNELS],
        "allowFineResidentPages": bool(allow_fine_resident),
        "pinnedInputs": [{"path": item["path"], "blake3": item["blake3"]} for item in pinned],
        # Non-loader provenance (the loader only reads the keys it calls at() on).
        "poseName": pose_name,
        "footprintMapping": mapping,
        "qualityCapture": str(Path(metadata_path).resolve()),
        "qualityCaptureSha256": T.sha256(metadata_path),
        "qualityCapturePayloadSha256": T.sha256(binary_path),
        "qualityCaptureEpochs": capture.counts(),
        "publicationManifest": str(Path(manifest).resolve()),
        "publicationSha256": T.sha256(manifest),
        "originalPageSetComplete": not pages["missing"],
        "missingOriginalPages": pages["missing"],
        "scope": ("Bounded same-epoch 128x128 lit/depth replay of the actual deferred path; "
                  "not full-frame calibration, per-pixel identity or TES acceptance"),
    }

    plan = {
        "generatedAt": datetime.datetime.now().astimezone().isoformat(),
        "pose": pose_name, "poseId": int(pose_id), "poseIdSource": pose_id_source,
        "posePlan": str(Path(plan_path).resolve()),
        "posePosition": position, "poseLookAt": look_at,
        "poseCommand": T.pose_command(position, look_at),
        "capture": str(Path(metadata_path).resolve()),
        "captureBinary": str(Path(binary_path).resolve()),
        "captureCounts": capture.counts(),
        "cameraDrift": drift,
        "mapping": mapping,
        "mappingNote": (mapping_note_extra + "; terrainFeedback is a fixed 128x128 target rendered with the actual camera and "
                        "a viewport/scissor equal to that target extent, so NDC is preserved and render "
                        "pixel p maps to feedback texel floor(p*128/renderExtent). `roi-center` keeps the "
                        "264 feedback texels whose centre falls inside the fixed ROI rect."),
        "feedbackTexels": len(texels),
        "footprint": {str(slot): identity for slot, identity in sorted(footprint.items())},
        "pageSource": str(Path(page_source).resolve()),
        "pageEntries": pages["entries"], "missingOriginalPages": pages["missing"],
        "publicationEquivalence": pages["equivalence"],
        "fragments": fragments,
        "inputClosureSha256": input_closure,
        "referencePageSetSha256": reference_set,
        "pinnedInputs": pinned,
        "runOutputDirectory": output_relative,
        "caseDirectory": str(case_directory.resolve()),
        "casePath": str((case_directory / "native-terrain-lit-replay-v1.json").resolve()),
        "gpuExecuted": False,
    }
    T.write(case_directory / "lit-case-plan.json", plan)
    T.write(Path(delivery) / ("plan-" + name + ".json"), plan)
    del prepared
    return case, plan, case_directory


def write_launch(case, plan, name, delivery, asset_root, tes_case, executable, sha256, command_delay=1):
    scene = T.read(Path(asset_root) / "config" / "config.json")["initScene"]
    argv = [str(Path(executable).resolve()), "--initial-scene", scene,
            "--worker-thread-count", "2", "--frame-limit", "30", "--present-mode", "immediate",
            "--no-dev-ui", "--validation", "--exit-after-frames", "180",
            "--console-command-delay", str(command_delay), "--console-command", plan["poseCommand"]]
    launch = {
        "gpuExecuted": False,
        "cwd": str(Path(asset_root).resolve()),
        "argv": argv,
        "environment": {"VULKANLEARN_TERRAIN_CAPTURE_CASE": str(Path(tes_case).resolve())},
        "executable": str(Path(executable).resolve()), "executableSha256": sha256,
        "litCase": str((Path(plan["caseDirectory"]) / "native-terrain-lit-replay-v1.json").resolve()),
        "scope": ("Explicit bounded diagnostic replay only; no P5 acceptance. A silent skip "
                  "(no native-lit-replay.json) means poseId/camera/fine-residency did not match."),
    }
    target = Path(plan["caseDirectory"]) / "launch.json"
    T.write(target, launch)
    T.write(Path(delivery) / ("launch-" + name + ".json"), launch)
    return target


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pose", default="junction_0_0")
    parser.add_argument("--name", default=None)
    parser.add_argument("--mapping", default="roi-center",
                        choices=("roi-center", "roi-intersect", "identity", "owners"))
    parser.add_argument("--capture", type=Path, default=None)
    parser.add_argument("--pose-id", type=int, default=None)
    parser.add_argument("--command-delay", type=int, default=1,
                        help="--console-command-delay frames; delays the pose arrival so the replay "
                             "frame can coincide with a captured quality frame")
    parser.add_argument("--pose-id-log", type=Path, default=None)
    parser.add_argument("--asset-root", type=Path, default=T.ASSET_ROOT)
    parser.add_argument("--manifest", type=Path, default=T.MANIFEST)
    parser.add_argument("--page-source", type=Path, default=T.PAGE_SOURCE)
    parser.add_argument("--extra-page-report", type=Path, action="append", default=None,
                        help="additional real page producer report (ROI page driver)")
    parser.add_argument("--fragment-dir", type=Path, default=T.FRAGMENT_DIR)
    parser.add_argument("--delivery", type=Path, default=T.DELIVERY)
    parser.add_argument("--tes-case", type=Path,
                        default=T.DELIVERY / "tes" / "junction-one-patch.json")
    parser.add_argument("--slot-capacity", type=int, default=None,
                        help="reference atlas layer count; must cover every observed slot")
    parser.add_argument("--allow-fine-resident", action="store_true",
                        help="lit-only near-field case: do not require zero non-root page residency")
    parser.add_argument("--allow-incomplete-pages", action="store_true")
    options = parser.parse_args()
    name = options.name or options.pose.replace("/", "-")
    pose_id, pose_id_source = resolve_pose_id(options.pose, options.pose_id, options.pose_id_log)
    case, plan, case_directory = build_case(
        name, options.pose, options.mapping, options.capture, pose_id, pose_id_source,
        options.asset_root, options.manifest, options.delivery, options.page_source,
        options.fragment_dir, options.extra_page_report, options.slot_capacity,
        options.allow_fine_resident)
    sha256 = T.sha256(T.LIT_EXE)
    launch = write_launch(case, plan, name, options.delivery, options.asset_root,
                          options.tes_case, T.LIT_EXE, sha256, options.command_delay)
    summary = {"case": None, "plan": plan["casePath"], "launch": str(launch.resolve()),
               "pages": len(case["originalGpuPages"]), "missing": len(case["missingOriginalPages"]),
               "footprint": len(case["actualRoiSlotFootprint"]),
               "poseId": case["poseId"], "gpuExecuted": False}
    case_path = case_directory / "native-terrain-lit-replay-v1.json"
    if not case["originalGpuPages"]:
        raise SystemExit("No real original-evaluator page could be bound; refusing to author a case")
    if not case["originalPageSetComplete"] and not options.allow_incomplete_pages:
        blocked = case_directory / "native-terrain-lit-replay-v1.blocked.json"
        blocked.write_text(json.dumps({**case, "kind":
                                       "native-terrain-lit-replay-v1-blocked",
                                       "blockedReason":
                                       "missing real original-evaluator pages for observed ROI slots"},
                                      indent=2, sort_keys=True) + "\n", encoding="utf-8")
        summary["blocked"] = str(blocked.resolve())
        T.write(Path(options.delivery) / ("blocked-" + name + ".json"),
                {"case": str(blocked.resolve()), "plan": plan["casePath"],
                 "missing": case["missingOriginalPages"],
                 "footprint": case["actualRoiSlotFootprint"],
                 "footprintMapping": options.mapping})
        print(json.dumps(summary))
        raise SystemExit(2)
    T.write(case_path, case)
    summary["case"] = str(case_path.resolve())
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
