"""CPU validation of the produced lit-ROI evidence chain.

Re-hashes every produced file, checks it against the loader's real expectations
(the `document.at(...)` keys of native-capture/terrainLitCase.cpp are extracted
from the source and required present with the right types), and writes
`final-delivery/lit/validation.json`. It never runs the renderer and never
claims the gate passes.
"""

import json
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np
import terrain_lit_evidence as T

FRAGMENT_SHA256 = {
    "deferredLighting.frag": "d751b2bbab7ef3d5ff98d229fc268cbd53d58af2c37d44e37e48b84eb2f42e3e",
    "toneMapping.frag": "924d7da2fc682f19eb68262fb3948f3c097be39b778c5a3a76751f8d5b8afc88",
}
P0_SHA256 = "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd"
MANIFEST_SHA256 = "1f92ee6690d1c026bc03aad135cbf196e8263929ba0f8388e7db3d9db6ec2e9e"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
# R14 original: ee90b97574f8f3bf4efb64060e802ed00f7de81db9cda40530fd9923763294e9.
# Rebuilt once for the four documented tool fixes in lit-replay-tool-fix.diff.
LIT_EXE_SHA256 = "f077991ce4e4d4a2e69bfc1ac00afcad5247714ab7f7a6ae17bf513af48c554c"


class Checks(object):
    def __init__(self):
        self.items = []

    def check(self, identifier, condition, detail=""):
        self.items.append({"id": identifier, "ok": bool(condition), "detail": detail})
        return bool(condition)

    def failed(self):
        return [item["id"] for item in self.items if not item["ok"]]


def loader_keys(source):
    text = Path(source).read_text(encoding="utf-8")
    return sorted(set(re.findall(r'document\.at\("([A-Za-z0-9_]+)"\)', text)))


def validate_case(case_path, plan_path, masks_path, expected_complete, checks, prefix,
                  allow_existing_output=False):
    case_path, plan_path, masks_path = Path(case_path), Path(plan_path), Path(masks_path)
    case, plan, masks = T.read(case_path), T.read(plan_path), T.read(masks_path)
    keys = loader_keys(T.ROOT / "tool/validation/terrain/native-capture/terrainLitCase.cpp")
    checks.check(prefix + ".loader-keys-extracted", len(keys) == 15,
                 "%d document.at() keys parsed from terrainLitCase.cpp" % len(keys))
    missing = [key for key in keys if key not in case]
    checks.check(prefix + ".loader-keys-present", not missing,
                 "missing=" + ",".join(missing) if missing else "all %d keys present" % len(keys))
    checks.check(prefix + ".kind", case["kind"] == "native-terrain-lit-replay-v1", case["kind"])
    checks.check(prefix + ".rect", case["rect"] == T.RECT,
                 "rect=%s (expanded fragments bake exactly this)" % case["rect"])
    checks.check(prefix + ".formats", case["actualAtlasFormats"] == [43, 37, 37],
                 "R8G8B8A8_SRGB,R8G8B8A8_UNORM,R8G8B8A8_UNORM")
    capacity = int(case["actualSlotCapacity"])
    footprint = [int(slot) for slot in case["actualRoiSlotFootprint"]]
    checks.check(prefix + ".capacity", 0 < capacity <= 512 and max(footprint) < capacity,
                 "capacity=%d covers max observed slot %d (P0Frozen cap 512)" % (capacity, max(footprint)))
    checks.check(prefix + ".footprint-bounds", 0 < len(footprint) == len(set(footprint)) <= 512 and
                 max(footprint) < capacity, "slots=%d max=%d" % (len(footprint), max(footprint)))
    checks.check(prefix + ".hash-shape", all(HEX64.match(case[key]) for key in
                 ("inputClosureSha256", "originalEvaluatorSha256", "referencePageSetSha256")),
                 "three 64-hex identities")
    checks.check(prefix + ".evaluator", case["originalEvaluatorSha256"] == T.ORIGINAL_EVALUATOR_SHA256,
                 "independent original evaluator identity")
    pages = case["originalGpuPages"]
    checks.check(prefix + ".pages-cover-footprint", sorted(int(row["actualPhysicalSlot"]) for row in pages)
                 == sorted(footprint), "pages=%d footprint=%d" % (len(pages), len(footprint)))
    checks.check(prefix + ".completeness", (len(pages) == len(footprint)) == expected_complete,
                 "expected complete=%s" % expected_complete)
    blake = T.Blake3()
    bad = []
    for row in pages:
        path = Path(row["path"])
        if not path.exists() or path.stat().st_size != T.PAGE_BYTES:
            bad.append(str(path))
            continue
        status = int.from_bytes(path.read_bytes()[T.PAGE_BYTES - 4:], "little")
        if status != 0 or blake.hash(path) != row["blake3"]:
            bad.append(str(path))
    checks.check(prefix + ".page-bytes", not bad,
                 "%d pages: %d bytes, uint32 status 0, BLAKE3 re-verified" % (len(pages), T.PAGE_BYTES))
    checks.check(prefix + ".reference-page-set-sha256",
                 T.page_set_sha256(pages) == case["referencePageSetSha256"],
                 "recomputed from the bound slots")
    pinned_bad = [item["path"] for item in case["pinnedInputs"]
                  if not Path(item["path"]).exists() or blake.hash(item["path"]) != item["blake3"]]
    checks.check(prefix + ".pinned-inputs", not pinned_bad,
                 "%d pinned inputs re-hashed" % len(case["pinnedInputs"]))
    closure = T.digest_document({"inputs": [[item["path"], item["blake3"]] for item in case["pinnedInputs"]],
                                 "identity": plan["identity"]}) if "identity" in plan else None
    if closure is None:
        identity = {"pose": plan["pose"], "poseId": plan["poseId"], "rect": T.RECT,
                    "cameraPosition": case["cameraPosition"], "footprint": sorted(footprint),
                    "originalPages": [row["blake3"] for row in pages],
                    "publicationSha256": T.sha256(T.MANIFEST)}
        closure = T.digest_document({"inputs": [[item["path"], item["blake3"]]
                                                for item in case["pinnedInputs"]], "identity": identity})
    checks.check(prefix + ".input-closure", closure == case["inputClosureSha256"],
                 "recomputed from pinned inputs + case identity")
    checks.check(prefix + ".output-fresh", allow_existing_output or not Path(case["output"]).exists(),
                 ("executed into " if allow_existing_output else "must not pre-exist: ") + case["output"])
    checks.check(prefix + ".camera", all(abs(float(a) - T._f32(b)) <= 0.02 for a, b in
                 zip(case["cameraPosition"], plan["posePosition"])),
                 "camera within 0.02 of the float32 capture-plan pose")
    checks.check(prefix + ".pose-id-source", isinstance(case["poseId"], int) and case["poseId"] > 0,
                 "poseId=%d from %s" % (case["poseId"], plan["poseIdSource"]))

    checks.check(prefix + ".masks-rect", masks["rect"] == case["rect"], "masks bound to the same ROI")
    checks.check(prefix + ".masks-camera-hash", HEX64.match(masks["cameraViewProjectionBlake3"]) is not None,
                 masks["viewProjectionSource"] if "viewProjectionSource" in masks else "observed")
    payload = masks_path.parent / masks["path"]
    checks.check(prefix + ".masks-payload", payload.exists() and T.sha256(payload) == masks["sha256"],
                 "masks.npz sha256 re-verified")
    with np.load(payload, allow_pickle=False) as archive:
        roi, domain = archive["terrainRoi"], archive["silhouetteDomain"]
    checks.check(prefix + ".masks-shape", roi.shape == domain.shape == (128, 128) and
                 roi.dtype == domain.dtype == np.bool_, "two boolean 128x128 arrays")
    checks.check(prefix + ".masks-nonempty", bool(roi.any()) and bool(domain.any()) and
                 bool(np.all(roi[domain])), "domain subset of the terrain ROI")
    checks.check(prefix + ".depth-clear", masks["deviceDepthClear"] == 1.0, "geometry clear depth 1.0")
    checks.check(prefix + ".masks-compare-schema",
                 all(key in masks for key in ("rect", "cameraViewProjectionBlake3", "path",
                                              "sha256", "deviceDepthClear")),
                 "schema consumed by compare_native_lit_roi.py:58-66")
    return case, plan, masks


def main():
    checks = Checks()
    delivery = T.DELIVERY
    checks.check("inputs.p0-frozen", T.sha256(T.ROOT / "config/terrainRvtP0Frozen.json") == P0_SHA256,
                 "frozen P0 identity")
    checks.check("inputs.manifest", T.sha256(T.MANIFEST) == MANIFEST_SHA256,
                 "p5g/C010r14 publication identity")
    for name, expected in FRAGMENT_SHA256.items():
        path = T.FRAGMENT_DIR / name
        checks.check("inputs.fragment." + name, path.exists() and T.sha256(path) == expected,
                     "expanded ROI fragment unchanged")
    hash_build = T.read(T.HASH_BUILD)
    checks.check("inputs.blake3-tool",
                 T.sha256(hash_build["executable"]) == hash_build["executableSha256"],
                 "native --hash binary identity")
    lit_sha = T.sha256(T.LIT_EXE)
    checks.check("inputs.lit-executable",
                 lit_sha == LIT_EXE_SHA256,
                 "native_terrain_lit_capture_game.exe SHA256 " + lit_sha +
                 " (R14 original ee90b975...; rebuilt with the four documented tool fixes)")

    tes_case = delivery / "tes" / "junction-one-patch.json"
    if checks.check("tes.case-present", tes_case.exists(), str(tes_case)):
        document = T.read(tes_case)
        checks.check("tes.kind", document.get("kind") == "native-terrain-test-capture-v1",
                     document.get("kind"))
        blake = T.Blake3()
        stale = [path for path, digest in document.get("actualFileBlake3", {}).items()
                 if not Path(path).exists() or blake.hash(path) != digest]
        checks.check("tes.pinned-bytes", not stale, "%d inputs re-hashed" %
                     len(document.get("actualFileBlake3", {})))
        checks.check("tes.case-shaped", "output" in document and "resourceRoot" in document,
                     "base TES case kept as an attempt record; the executed one is tes-r11")

    far = delivery / "case-far-plumbing-smoke"
    far_plan = T.read(far / "lit-case-plan.json")
    checks.check("attempts.far-recorded", (far / "native-terrain-lit-replay-v1.json").exists(),
                 "far-pose attempt kept for provenance; not a validated gate case")
    validated = ["far-plumbing-smoke"]
    executed_name = "junction-0-0-r9"
    for name in ("junction-0-0-r14",):
        case_path = delivery / ("case-" + name) / "native-terrain-lit-replay-v1.json"
        plan_path = delivery / ("case-" + name) / "lit-case-plan.json"
        masks_path = delivery / ("masks-" + name) / "masks.json"
        prefix = "executed_case"
        checks.check(prefix + ".artifacts-present", case_path.exists() and plan_path.exists() and
                     masks_path.exists(), str(case_path.parent))
        if case_path.exists() and plan_path.exists() and masks_path.exists():
            validate_case(case_path, plan_path, masks_path, True, checks, prefix,
                          allow_existing_output=True)
            validated.append(name)
    attempt_dirs = sorted(path.parent.name for path in delivery.glob("case-junction-0-0*/native-terrain-lit-replay-v1.json"))
    checks.check("attempts.recorded", len(attempt_dirs) >= 8,
                 "%d authored junction attempts kept (r2..r9): %s" % (len(attempt_dirs), attempt_dirs))
    junction = delivery / "case-junction-0-0"
    blocked_path = junction / "native-terrain-lit-replay-v1.blocked.json"
    checks.check("junction.case-withheld",
                 not (junction / "native-terrain-lit-replay-v1.json").exists() and blocked_path.exists(),
                 "loader-invalid kind is used so run_native_lit_roi.ps1 refuses the incomplete case")
    blocked = T.read(blocked_path)
    junction_plan = T.read(junction / "lit-case-plan.json")
    checks.check("junction.footprint", len(junction_plan["footprint"]) == 8 and
                 len(blocked["originalGpuPages"]) == 4 and len(blocked["missingOriginalPages"]) == 4,
                 "8 observed ROI slots, 4 real pages, 4 missing")
    driver = delivery / "page-driver" / "build-plan.json"
    if checks.check("driver.plan-present", driver.exists(), str(driver)):
        document = T.read(driver)
        requests = T.read(document["requests"])["requests"]
        missing = {(row["surfaceId"], row["mip"], row["x"], row["y"])
                   for row in blocked["missingOriginalPages"]}
        requested = {(row["surfaceId"], row["mip"], row["x"], row["y"]) for row in requests}
        checks.check("driver.requests-match", requested == missing,
                     "request list is exactly the four missing ROI addresses")
        checks.check("driver.status", document["status"] == "ready-to-build",
                     "no missing frozen objects: " + json.dumps(document["problems"]))
        checks.check("driver.not-executed", document["compiled"] is False and
                     document["gpuExecuted"] is False, "plan only; nothing was built or run")

    # ---- executed GPU evidence (see LIT-SAME-FRAME-RESULTS.md) ---------------
    run_root = delivery / "runs" / "junction-0-0-r14-r1"
    metadata_path = run_root / "raw" / "native-lit-replay.json"
    comparison_path = run_root / "lit-roi-comparison.json"
    executed_masks = delivery / "masks-junction-0-0-r14" / "masks.json"
    repeat_root = delivery / "runs" / "junction-0-0-r13-r1"
    repeat_metadata = repeat_root / "raw" / "native-lit-replay.json"
    repeat_comparison = repeat_root / "lit-roi-comparison.json"
    executed = {"run": str(run_root), "repeat": str(repeat_root)}
    if checks.check("executed.replay-present", metadata_path.exists(), str(metadata_path)):
        native = T.read(metadata_path)
        masks = T.read(executed_masks)
        executed["nativeMetadataSha256"] = T.sha256(metadata_path)
        executed["frame"] = native["frame"]
        executed["submissionEpoch"] = native["submissionEpoch"]
        executed["completedEpoch"] = native["completedEpoch"]
        executed["poseId"] = native["poseId"]
        checks.check("executed.gpu", native.get("gpuExecuted") is True and
                     native["submissionEpoch"] > 0 and native["completedEpoch"] >= native["submissionEpoch"],
                     "frame=%d submission=%d completed=%d poseId=%s" %
                     (native["frame"], native["submissionEpoch"], native["completedEpoch"], native["poseId"]))
        checks.check("executed.camera", native["cameraViewProjectionBlake3"] ==
                     masks["cameraViewProjectionBlake3"],
                     "engine-derived masks binding equals the executed frame: " +
                     native["cameraViewProjectionBlake3"][:16])
        checks.check("executed.rect", native["rect"] == masks["rect"] == T.RECT, str(native["rect"]))
        checks.check("executed.draws", len(native["actualDraws"]) == 256,
                     "%d actual draws replayed" % len(native["actualDraws"]))
        blake = T.Blake3()
        rehash = {}
        for channel in native["channels"]:
            path = run_root / "raw" / channel["path"]
            rehash[channel["role"] + "/" + channel["channel"]] = blake.hash(path)
        executed["rehash"] = rehash
        checks.check("executed.channels", len(native["channels"]) == 4 and all(
                     (run_root / "raw" / channel["path"]).stat().st_size == channel["bytes"] and
                     rehash[channel["role"] + "/" + channel["channel"]] == channel["payloadBlake3"]
                     for channel in native["channels"]),
                     "4 x 65,536 B channels re-hashed with native-tes-cpu.exe --hash")
        if checks.check("executed.comparison-present", comparison_path.exists(), str(comparison_path)):
            report = T.read(comparison_path)
            executed["comparisonSha256"] = T.sha256(comparison_path)
            executed["lit"] = report["lit"]
            executed["silhouette"] = report["silhouette"]
            executed["depth"] = report["depth"]
            executed["diagnosticThresholds"] = report["diagnosticThresholds"]
            checks.check("executed.comparison-identities",
                         report["nativeMetadataSha256"] == T.sha256(metadata_path) and
                         report["fixedMasksSha256"] == T.sha256(executed_masks),
                         "comparison bound to the executed metadata and fixed masks")
            rehash_ok = all(
                report["payloads"]["('%s', '%s')" % tuple(key.split("/"))]["blake3"] ==
                executed["rehash"][key] for key in executed["rehash"])
            checks.check("executed.hash-build-rehash-ran", rehash_ok,
                         "compare_native_lit_roi.py --hash re-derivation matches all four recorded channels (no skipped channel)")
            thresholds = report["diagnosticThresholds"]
            checks.check("executed.all-thresholds", all(thresholds.values()),
                         "MAE %.4f RMSE %.4f P99 %.4f bytes; XOR %.6f; depth maxAbs %.4f; %s" %
                         (report["lit"]["maeBytes"], report["lit"]["rmseBytes"],
                          report["lit"]["p99ErrorBytes"], report["silhouette"]["xorFraction"],
                          report["depth"]["maxAbs"], thresholds))
            checks.check("executed.not-claiming-overall", report["overallP5Passed"] is False,
                         "bounded diagnostic gate only; overallP5Passed stays false")
            candidate = [c for c in native["channels"] if c["role"] == "candidate" and c["channel"] == "lit"][0]
            reference = [c for c in native["channels"] if c["role"] == "reference" and c["channel"] == "lit"][0]
            checks.check("executed.branches-identical", candidate["payloadBlake3"] == reference["payloadBlake3"],
                         "candidate and reference lit payloads are the same bytes")
    if checks.check("determinism.repeat-present", repeat_metadata.exists(), str(repeat_metadata)):
        first, second = T.read(metadata_path), T.read(repeat_metadata)
        executed["repeatMetadataSha256"] = T.sha256(repeat_metadata)
        checks.check("determinism.frame-epoch-pose",
                     (first["frame"], first["submissionEpoch"], first["poseId"]) ==
                     (second["frame"], second["submissionEpoch"], second["poseId"]),
                     "both runs at frame %d epoch %d poseId %s" %
                     (second["frame"], second["submissionEpoch"], second["poseId"]))
        checks.check("determinism.channels", first["channels"] == second["channels"],
                     "all four payload BLAKE3s identical across two GPU runs")
        case_a = T.read(delivery / "case-junction-0-0-r13" / "native-terrain-lit-replay-v1.json")
        case_b = T.read(delivery / "case-junction-0-0-r14" / "native-terrain-lit-replay-v1.json")
        checks.check("determinism.semantic-closure",
                     case_a["actualRoiSlotFootprint"] == case_b["actualRoiSlotFootprint"] == [0, 1, 4, 5] and
                     case_a["referencePageSetSha256"] == case_b["referencePageSetSha256"] and
                     [p["blake3"] for p in case_a["originalGpuPages"]] ==
                     [p["blake3"] for p in case_b["originalGpuPages"]],
                     "identical slot set, page bytes and reference-page-set hash; inputClosureSha256 differs only because it hashes per-case page paths")
        checks.check("determinism.metrics", repeat_comparison.exists() and
                     T.read(repeat_comparison)["lit"] == T.read(comparison_path)["lit"] and
                     T.read(repeat_comparison)["silhouette"] == T.read(comparison_path)["silhouette"],
                     "identical lit/silhouette metrics across the two runs")
    blocked = delivery / "runC-stderr.log"
    blocked_text = ""
    if blocked.exists():
        payload = blocked.read_bytes()
        for encoding in ("utf-8", "utf-16"):
            try:
                blocked_text = payload.decode(encoding)
                break
            except (UnicodeDecodeError, ValueError):
                continue
    checks.check("delayed-pose.blocked-evidenced", blocked.exists() and
                 "TES active parameters" in blocked_text and "witness invalid" in blocked_text,
                 "delayed (warm-frame) replay aborts in the combined TES adapter, so the warm same-frame footprint cannot be captured this way")

    launch = T.read(far_plan["caseDirectory"] + "/launch.json") \
        if (Path(far_plan["caseDirectory"]) / "launch.json").exists() else None
    if checks.check("run.fixture", launch is not None, "generated launch fixture"):
        checks.check("run.argv", isinstance(launch.get("argv"), list) and len(launch["argv"]) > 1,
                     "argv for run_native_lit_roi.ps1")
        checks.check("run.cwd", Path(launch["cwd"]).is_dir(), launch["cwd"])
        checks.check("run.tes-env", Path(launch["environment"]["VULKANLEARN_TERRAIN_CAPTURE_CASE"]).exists(),
                     "TES case bound in the fixture environment")
        checks.check("run.executable", T.sha256(Path(launch["executable"])) == launch["executableSha256"],
                     "fixture executable hash matches")

    document = {
        "generatedAt": __import__("datetime").datetime.now().astimezone().isoformat(),
        "scope": ("Authored inputs plus the executed GPU evidence: two same-frame-bound native "
                  "lit/depth replays whose reference pages come from the replay frame's own TVQ1 "
                  "residency, their comparisons, the TES R16 oracle, and the delayed-pose blocker. "
                  "All lit thresholds pass at the cold root-mip frame; the warm near-field frame "
                  "is not yet reachable because the combined TES adapter aborts on it."),
        "overallPassed": False,
        "gpuExecuted": True,
        "executed": executed,
        "checks": checks.items,
        "failed": checks.failed(),
        "completeRunnableCase": str((far / "native-terrain-lit-replay-v1.json").resolve()),
        "casesValidated": validated,
        "recommendedCase": str((delivery / "case-junction-0-0-r2" /
                                "native-terrain-lit-replay-v1.json").resolve()),
        "recommendedCaseBlockedOnPages": str(blocked_path.resolve()),
        "remainingGaps": [
            "the passing lit frame is root-only (4 x mip13 pages, cold frame 2); P0 marks "
            "rootOnlyIsReady=false, so this proves same-frame binding, not production near-field lit",
            "4 mip1 corner pages have no real original-evaluator bytes; page-driver/build-plan.json "
            "defines the exact build+GPU run that produces them",
            "poseId is predicted (2) from a single `camera pose` command; confirm from the run's "
            "runtime evidence and regenerate with --pose-id if the replay silently skips",
            "cameraViewProjectionBlake3 is engine-derived; if the run's value differs by 1 ULP the "
            "masks must be rebound with --view-projection-source observed",
            "lit MAE/RMSE/P99 fail because the bound reference slots come from a warm s2-cov-w2 "
            "capture while the replay frame is a cold frame-2 allocation: physical slot indices are "
            "allocation-order dependent, so the candidate page table does not point at the bound "
            "pages (depth payloads are byte-identical, silhouette XOR 0)",
            "closing that needs a TVQ1 capture exported from the replay run itself "
            "(VULKANLEARN_RVT_QUALITY_DIR) with the pose command delayed to a captured frame, then "
            "pages baked for exactly those slots",
            "candidate-vs-actual-frame calibration, per-pixel identity/UV/worldpos/LOD remain "
            "outside this bounded comparison",
        ],
    }
    target = delivery / "validation.json"
    T.write(target, document)
    print(json.dumps({"output": str(target), "checks": len(checks.items),
                      "failed": document["failed"], "overallPassed": False}))
    raise SystemExit(2 if checks.failed() else 0)


if __name__ == "__main__":
    main()
