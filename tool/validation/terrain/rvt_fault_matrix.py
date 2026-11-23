"""Isolated RVT fault preparation and explicitly scheduled installed Game runs.

Default --plan is CPU-only and never spawns a process. --prepare also exercises
copy/patch/restore on disposable complete packages. --verify-installations runs
only the native CPU exporter verifier. --execute launches real Game
processes serially and reports missing native evidence as incomplete (exit 2).
No source package, content-addressed name, manifest digest or P0 cap is repaired.
"""

import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
FROZEN_SHA256 = "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd"
CHUNK = 1024 * 1024
JSON_LIMIT = 16 * CHUNK
FILE_CASES = ("root_payload", "production_image", "production_raw_kind3",
              "v1_version", "legacy_material_switch")
HOOKS = {
    "candidate_retention": {
        "api": "TestingService.QueueRvtCandidateFault(candidateScene, objectId, byteOffset, xorMask)",
        "owner": "WorldCandidatePreparer / WorldGraphTransactionCoordinator",
        "observe": ["rejection(kind,code,objectId)", "world(candidateResult,activeGenerationBefore,"
                    "activeGenerationAfter)", "old World renders after rejection"],
        "reason": "loadworld exists; no atomic after-active-commit mutation/admission barrier or "
                  "complete candidate retention evidence is exposed by the current console"},
    "max_budget": {
        "api": "TestingService.QueueRvtAdmissionFault(candidateScene, ledger, extraRequiredBytes)",
        "owner": "TerrainRvtBudget cross-World admission owner",
        "observe": ["admission(requiredBytes,capBytes,frozenSha256,candidateResult)",
                    "world(activeGenerationBefore,activeGenerationAfter)", "candidate allocations retired"],
        "reason": "Inject demand above the unchanged frozen cap; do not edit frozen JSON"},
    "producer_status": {
        "api": "TestingService.ArmRvtProductionFault(pageIdentity, nonzeroStatus, once)",
        "owner": "production status consumption after real completed fence",
        "observe": ["production-failure(status,attempts,maxAttempts,backoffTicks,publishedChannels,"
                    "slot,writeEpoch)", "slot-retirement(completedWriteEpoch,reuseEpoch)"],
        "reason": "No terrain production-status injection console command found"},
    "submit_failure": {
        "api": "TestingService.ArmRvtSubmitFault(pageIdentity, VkResult, once)",
        "owner": "RHI submission result / production abort and retirement owner",
        "observe": ["production-failure(submitResult,publishedChannels,slot,writeEpoch)",
                    "slot-retirement(completedWriteEpoch,reuseEpoch)"],
        "reason": "Pipeline/world transaction test faults are not an RVT submit failure"},
    "late_revision": {
        "api": "TestingService.HoldRvtCompletion(token); AdvanceRvtRevision(surfaceId); "
               "ReleaseRvtCompletion(token)",
        "owner": "production token validation / revision / retirement owner",
        "observe": ["actual old/new revision and token", "stale completion rejected",
                    "no old publication into new page table"],
        "reason": "No deterministic in-flight revision barrier exposed"},
    "slot_reuse": {
        "api": "TestingService.HoldRvtRetirement(slot,writeEpoch); ReleaseRvtRetirement(token)",
        "owner": "residency lease / frame epoch / slot allocator",
        "observe": ["reservation blocked before completion", "slot-retirement(completedWriteEpoch,"
                    "reuseEpoch)", "reuseEpoch > completedWriteEpoch"],
        "reason": "No deterministic retirement hold/release command exposed"},
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    require(path.stat().st_size <= JSON_LIMIT, f"JSON exceeds bounded reader: {path}")
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def identity(path):
    return {"bytes": path.stat().st_size, "sha256": sha256(path)}


def regular_tree(root):
    require(root.is_dir(), f"Not a directory: {root}")
    for path in [root, *root.rglob("*")]:
        require(not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(),
                f"Links/junctions forbidden in disposable package: {path}")
        attributes = getattr(path.stat(), "st_file_attributes", 0)
        require(not (attributes & 0x400), f"Reparse point: {path}")
        if path.is_file():
            require(path.stat().st_nlink == 1, f"Hard-linked package file forbidden: {path}")


def contained(root, relative):
    logical = Path(relative)
    require(not logical.is_absolute() and not logical.drive and ".." not in logical.parts,
            f"Unsafe relative path: {relative}")
    path = (root / logical).resolve(strict=True)
    require(path.is_relative_to(root.resolve()) and path.is_file(), f"Escaped/missing file: {relative}")
    return path


def inventory(root):
    return {path.relative_to(root).as_posix(): identity(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def layout(package):
    config = read_json(package / "config/config.json")
    require(config.get("resourcePath") == "resources", "Requires a complete independent installed Game")
    require((package / "main_game.exe").is_file(), "Missing installed main_game.exe")
    require((package / "game-export-manifest.json").is_file(), "Missing export manifest")
    frozen = package / "config/terrainRvtP0Frozen.json"
    require(sha256(frozen) == FROZEN_SHA256, "Frozen P0 bytes changed")
    require(Path(str(frozen) + ".sha256").read_text().split()[0] == FROZEN_SHA256,
            "Frozen P0 sidecar differs")
    resources = package / "resources"
    scene = read_json(contained(resources, config["initScene"]))
    placements = [row for row in scene["objects"] if row.get("type") == "terrain"]
    require(placements, "Scene has no terrain")
    terrains = [contained(resources, row["terrainPath"]) for row in placements]
    manifests = {read_json(path)["compositedVT"]["packagePath"] for path in terrains}
    require(len(manifests) == 1, "Expected one shared World publication")
    manifest = contained(resources, manifests.pop())
    world = read_json(manifest)
    require(world["version"] == 2, "Baseline must be V2")
    ids = {row["surfaceId"] for row in world["production"]["surfaces"]}
    require(ids == {"terrain|" + row["name"] for row in placements}, "Placement identity mismatch")
    return config, manifest, world


def patch_record(package, target, offset, before, after, label, **details):
    require(before != after and before, "Patch must change bytes")
    require(len(before) == len(after) or
            (offset == 0 and len(before) == target.stat().st_size <= JSON_LIMIT),
            "Resizing requires a bounded whole JSON replacement")
    require(0 <= offset <= target.stat().st_size - len(before), "Patch outside file")
    with target.open("rb") as stream:
        stream.seek(offset)
        require(stream.read(len(before)) == before, "Patch before bytes differ")
    return {"case": label, "path": target.relative_to(package).as_posix(), "offset": offset,
            "beforeHex": before.hex(), "afterHex": after.hex(),
            "original": identity(target), **details}


def xor_patch(package, target, offset, label, **details):
    with target.open("rb") as stream:
        stream.seek(offset)
        before = stream.read(1)
    require(len(before) == 1, "Missing mutation byte")
    return patch_record(package, target, offset, before, bytes([before[0] ^ 1]), label, **details)


def json_patch(package, path, changed, label, **details):
    require(path.stat().st_size <= JSON_LIMIT, "JSON mutation exceeds bound")
    before = path.read_bytes()
    after = json.dumps(changed, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    after += b" " * max(0, len(before) - len(after))
    return patch_record(package, path, 0, before, after, label, **details)


def mutations(package):
    config, manifest, world = layout(package)
    production = world["production"]
    root = world["roots"][0]
    page = next(row for row in world["pages"]
                if all(row[key] == root[key] for key in ("surfaceId", "mip", "x", "y")))
    payload = contained(manifest.parent, world["package"])
    require(page["size"] > 0 and page["offset"] >= 16 and
            page["offset"] + page["size"] <= payload.stat().st_size, "Invalid root range")
    records = [xor_patch(package, payload, page["offset"], "root_payload", page=page)]
    bindings = production["surfaces"][0]["inputs"]
    require(isinstance(bindings, list) and len(bindings) == 15,
            "Native V2 requires the exact 15-entry input binding array")
    image_id = bindings[5]
    image = next(row for row in production["objects"] if row["objectId"] == image_id)
    require(image["kind"] == 0, "layerAlbedo must reference a real image input")
    raw = next((row for row in production["objects"] if row["kind"] == 3), None)
    for label, entry in (("production_image", image), ("production_raw_kind3", raw)):
        if entry is not None:
            records.append(xor_patch(package, contained(manifest.parent, entry["path"]), 0,
                                     label, objectId=entry["objectId"], objectKind=entry["kind"],
                                     requiredBy="V2 Store.ValidateInputs(all objects, including kind=3)"))
    require(manifest.stat().st_size <= JSON_LIMIT, "Manifest mutation exceeds bound")
    original = manifest.read_bytes()
    matches = list(re.finditer(rb'"version"\s*:\s*(2)(?=\s*[,}])', original))
    require(len(matches) == 1, "Ambiguous V2 manifest version")
    records.append(patch_record(package, manifest, matches[0].start(1), b"2", b"1", "v1_version"))
    material = contained(manifest.parent, production["surfaces"][0]["runtimeMaterialInstancePath"])
    changed = read_json(material)
    require(changed.get("material") == "shader/glsl/M_terrain.json",
            "Expected actual archived terrain MI")
    changed.setdefault("parameters", {})["u_terrainLayerMode"] = 0
    records.append(json_patch(package, material, changed, "legacy_material_switch",
                              field="parameters.u_terrainLayerMode"))
    return config, records


def virtual_hash(path, patch):
    offset = patch["offset"]
    after = bytes.fromhex(patch["afterHex"])
    before = bytes.fromhex(patch["beforeHex"])
    if len(after) != len(before):
        require(offset == 0 and len(before) == path.stat().st_size <= JSON_LIMIT,
                "Unbounded resizing forbidden")
        return hashlib.sha256(after).hexdigest()
    digest = hashlib.sha256()
    position = 0
    with path.open("rb") as stream:
        while block := stream.read(CHUNK):
            start = max(position, offset)
            end = min(position + len(block), offset + len(after))
            if start < end:
                block = (block[:start - position] + after[start - offset:end - offset] +
                         block[end - position:])
            digest.update(block)
            position += len(block)
    return digest.hexdigest()


def copy_package(source, destination):
    require(not destination.exists(), "Disposable package already exists")
    require(not destination.is_relative_to(source) and not source.is_relative_to(destination),
            "Disposable package overlaps original")
    regular_tree(source)
    if os.name == "nt":
        longest = max(len(str(destination / path.relative_to(source)))
                      for path in source.rglob("*") if path.is_file())
        require(longest < 260, f"Windows package path would reach {longest} chars; "
                "select a fresh shorter --private-root (no copy started)")
    shutil.copytree(source, destination, copy_function=shutil.copyfile)
    regular_tree(destination)
    require(inventory(source) == inventory(destination), "Independent copy bytes differ")


@contextmanager
def patched(private, patch, evidence):
    ownership = read_json(evidence / "private-copy.json")
    require(ownership["privatePackage"] == str(private.resolve()), "Not a runner-owned copy")
    require(Path(ownership["sourcePackage"]).resolve() != private.resolve(), "Refusing original package")
    target = contained(private, patch["path"])
    require(target.stat().st_nlink == 1, "Mutation target is shared by hard link")
    require(identity(target) == patch["original"], "Copy no longer matches planned baseline")
    before = bytes.fromhex(patch["beforeHex"])
    after = bytes.fromhex(patch["afterHex"])
    receipt = dict(patch, privatePath=str(target), restored=False)
    save(evidence / "mutation.json", receipt)
    try:
        with target.open("r+b") as stream:
            stream.seek(patch["offset"])
            require(stream.read(len(before)) == before, "Mutation preimage mismatch")
            stream.seek(patch["offset"])
            stream.write(after)
            if len(after) != len(before):
                stream.truncate(len(after))
            stream.flush()
            os.fsync(stream.fileno())
        receipt["mutated"] = identity(target)
        require(receipt["mutated"]["sha256"] == patch["mutatedSha256"], "Mutation hash mismatch")
        save(evidence / "mutation.json", receipt)
        yield receipt
    finally:
        with target.open("r+b") as stream:
            stream.seek(patch["offset"])
            stream.write(before)
            if len(after) != len(before):
                stream.truncate(len(before))
            stream.flush()
            os.fsync(stream.fileno())
        require(identity(target) == patch["original"], "Exact restoration failed")
        receipt["restored"] = True
        receipt["restoredIdentity"] = identity(target)
        save(evidence / "mutation.json", receipt)


def process(argv, cwd, evidence, label, timeout, env=None):
    stdout = evidence / (label + ".stdout.log")
    stderr = evidence / (label + ".stderr.log")
    result = {"argv": [str(value) for value in argv], "cwd": str(cwd), "exitCode": None,
              "timedOut": False, "launchError": None}
    started = time.monotonic()
    with stdout.open("wb") as output, stderr.open("wb") as error:
        try:
            child = subprocess.Popen(result["argv"], cwd=cwd, stdout=output, stderr=error, env=env)
            result["pid"] = child.pid
            try:
                result["exitCode"] = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                result["timedOut"] = True
                child.kill()
                result["exitCode"] = child.wait()
        except OSError as error:
            result["launchError"] = str(error)
    result["elapsedSeconds"] = time.monotonic() - started
    result["stdout"] = dict(path=str(stdout), **identity(stdout))
    result["stderr"] = dict(path=str(stderr), **identity(stderr))
    save(evidence / (label + ".process.json"), result)
    return result


def events_from(text):
    events, errors = [], []
    run_id = None
    last_sequence = -1
    for line in text.splitlines():
        if "[RVT_EVIDENCE] " in line:
            try:
                event = json.loads(line.split("[RVT_EVIDENCE] ", 1)[1])
                require(isinstance(event, dict), "Event is not an object")
                require(event.get("protocol") == 1 and isinstance(event.get("event"), str),
                        "Missing native event protocol/name")
                require(isinstance(event.get("runId"), str) and event["runId"], "Missing native runId")
                if run_id is None:
                    run_id = event["runId"]
                require(event["runId"] == run_id, "Mixed native runIds")
                for key in ("sequence", "frame", "worldGeneration"):
                    require(type(event.get(key)) is int and event[key] >= 0,
                            f"Invalid event integer: {key}")
                require(event["sequence"] > last_sequence, "Non-increasing native event sequence")
                last_sequence = event["sequence"]
                require(type(event.get("monotonicMs")) in (int, float) and
                        math.isfinite(event["monotonicMs"]) and event["monotonicMs"] >= 0,
                        "Invalid native monotonic timestamp")
                events.append(event)
            except (ValueError, TypeError) as error:
                errors.append(str(error))
    return events, errors


def log_text(result):
    return "\n".join(Path(result[key]["path"]).read_text(encoding="utf-8", errors="replace")
                     for key in ("stdout", "stderr"))


def classify_process(result, text, expected):
    if result["timedOut"] or result["launchError"]:
        return "timeout-or-launch-failure"
    code = result["exitCode"]
    if code not in (0, 1):
        return "crash-or-unexpected-exit"
    if re.search(r"VK_ERROR_DEVICE_LOST|VUID-|access violation|segmentation fault|Shader cache miss:",
                 text, re.I):
        return "runtime-error"
    if expected == "reject":
        return "clean-rejection" if code == 1 and "EngineLoop teardown complete" in text else "incomplete"
    return "clean-success" if code == 0 and "EngineLoop teardown complete" in text else "incomplete"


def verify_rejection(result):
    text = log_text(result)
    if result["timedOut"] or result["launchError"] or result["exitCode"] != 1:
        return "not-a-verified-rejection"
    try:
        records = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
    except ValueError:
        records = []
    if any(row.get("ok") is False and isinstance(row.get("error"), str) for row in records):
        return "native-installation-rejected"
    return "incomplete-native-diagnostic"


def installation_layer(result):
    text = log_text(result)
    if "Package file digest mismatch:" in text:
        return "export-inventory-digest"
    if "VT production input integrity mismatch" in text:
        return "v2-store-production-input-digest"
    return "other-native-diagnostic-see-raw-logs"


def assess(case, game, install, patch):
    text = log_text(game)
    events, errors = events_from(text)
    expected = "recover" if case == "root_payload" else "reject"
    classification = classify_process(game, text, expected)
    result = {"status": "incomplete", "runtimeClassification": classification,
              "installationClassification": verify_rejection(install), "events": events,
              "installationFailureLayer": installation_layer(install),
              "malformedEvents": errors, "acceptancePassed": False}
    if classification in ("crash-or-unexpected-exit", "runtime-error", "timeout-or-launch-failure"):
        result["status"] = "failed"
        return result
    if errors or result["installationClassification"] != "native-installation-rejected":
        return result
    if case == "root_payload" and classification == "clean-success":
        from accept_game_delivery import validate_recovery
        from rvt_delivery import IncompleteEvidence
        try:
            result["recovery"] = validate_recovery(text, patch["page"], True)
            result.update(status="passed", acceptancePassed=True)
        except (IncompleteEvidence, ValueError, KeyError) as error:
            result["missingEvidence"] = str(error)
    elif case in ("production_image", "production_raw_kind3"):
        result["missingEvidence"] = "Startup reject is insufficient: need object-bound rejection and " \
            "actual candidate rejection with old active World retained and rendering"
    else:
        rejection = [row for row in events if row.get("event") == "rejection"]
        required_kind = "v1" if case == "v1_version" else "legacy-switch"
        if classification == "clean-rejection" and any(
                row.get("kind") == required_kind and
                str(row.get("path", "")).replace("\\", "/").endswith(patch["path"]) and
                type(row.get("candidateGeneration")) is int and
                type(row.get("activeGeneration")) is int for row in rejection):
            result.update(status="passed", acceptancePassed=True)
    return result


def hook_inventory():
    files = ["source/debugConsoleProcessCommand.cpp", "source/engine/runtimeCommand.h",
             "source/engine/runtimeCommandExecutorExecuteCommand.cpp",
             "source/engine/testing/runtimeValidationServices.h",
             "source/engine/testing/worldTransactionRuntimeTests.cpp",
             "source/render/resource/terrainCompositedService.cpp",
             "source/render/resource/terrainCompositedProductionGpu.cpp"]
    return {"existing": ["terrainvt stats (read-only)", "loadworld/loadscene <path>",
                         "--world-graph-transaction-test (generic rollback; not RVT injection)"],
            "missing": HOOKS, "sourceBindings": {name: identity(ROOT / name) for name in files},
            "integrationRule": "Euclid implements typed RuntimeCommandBus commands dispatched to "
                               "TestingService; one-shot faults at actual owners/safe points, no new "
                               "runtime test flags, no per-frame if, no cap/validator relaxation"}


def game_argv(private, config, options):
    return [str(private / "main_game.exe"), "--initial-scene", config["initScene"],
            "--worker-thread-count", str(options.worker), "--frame-limit", "30",
            "--present-mode", "immediate", "--no-dev-ui", "--validation",
            "--exit-after-frames", str(options.frames),
            "--console-command-delay", "60", "--console-command", "terrainvt stats"]


def run(options):
    package = options.package.resolve(strict=True)
    regular_tree(package)
    output = options.output.resolve()
    private_root = (options.private_root or output / "private").resolve()
    require(not output.exists(), "Use fresh evidence directory")
    require(not output.is_relative_to(package) and not package.is_relative_to(output),
            "Evidence overlaps source package")
    require(not private_root.exists(), "Use a fresh private-root")
    require(not private_root.is_relative_to(package) and not package.is_relative_to(private_root),
            "Private-root overlaps original package")
    require(not output.is_relative_to(private_root), "Private-root cannot contain evidence")
    config, patches = mutations(package)
    selected = set(options.case or (*FILE_CASES, *HOOKS))
    patches = [patch for patch in patches if patch["case"] in selected]
    for patch in patches:
        patch["mutatedSha256"] = virtual_hash(contained(package, patch["path"]), patch)
    mode = ("execute" if options.execute else "verify-installations" if options.verify_installations
            else "prepare" if options.prepare else "plan")
    world = layout(package)[2]
    if mode == "execute":
        require(len(world["production"]["surfaces"]) == 16,
                "Game execution requires the real Full16 installed World, never a small CPU fixture")
    report = {"status": "planned-incomplete", "mode": mode, "rendererStarted": False,
              "acceptancePassed": False, "sourcePackage": str(package),
              "privateRoot": str(private_root),
              "sourceExportManifest": identity(package / "game-export-manifest.json"),
              "binary": identity(package / "main_game.exe"), "frozenSha256": FROZEN_SHA256,
              "runner": identity(Path(__file__)),
              "worker": options.worker, "requiredWorkers": [1, 2],
              "weightSources": sorted({row["weightSource"] for row in layout(package)[2]["production"]["surfaces"]}),
              "patches": patches, "hooks": hook_inventory(),
              "notApplicable": {"cache_deleted": "optional disk fine cache disabled; N/A, never pass",
                                "cache_corrupt": "optional disk fine cache disabled; N/A, never pass"},
              "coverageMissing": sorted(selected - {patch["case"] for patch in patches}),
              "requiredNativeHookCases": list(HOOKS),
              "scope": "Single package/worker; final acceptance requires complete formal Game packages "
                       "for both weight modes and workers 1/2, including all missing hook cases",
              "rawModePolicy": "All V2 production objects are required, including archived kind=3 "
                               "author-source bytes; do not skip them based on rawMode or image bindings",
              "installationPolicy": "Unchanged content-addressed paths/digests; preserve native "
                                    "diagnostic. Whole-package inventory rejection alone does not prove "
                                    "Store input rejection or Game recovery.",
              "results": []}
    report["plannedRuns"] = [
        {"case": patch["case"], "privatePackage": str(private_root / str(index)),
         "argv": game_argv(private_root / str(index), config, options)}
        for index, patch in enumerate(patches)]
    output.mkdir(parents=True)
    save(output / "matrix.json", report)
    if mode == "plan":
        return 0
    validator = options.validator.resolve(strict=True) if options.validator else None
    native = mode in ("execute", "verify-installations")
    require(not native or (validator and validator.name.lower() == "vl_export_game.exe"),
            "Native runs require --validator vl_export_game.exe")
    if native:
        report["validator"] = dict(path=str(validator), **identity(validator))
    before = inventory(package)
    save(output / "source-before.json", before)
    private_root.mkdir(parents=True)
    try:
        for index, patch in enumerate(patches):
            evidence = output / patch["case"]
            evidence.mkdir()
            private = private_root / str(index)
            copy_package(package, private)
            save(evidence / "private-copy.json", {
                "privatePackage": str(private.resolve()), "sourcePackage": str(package),
                "sourceExportManifest": report["sourceExportManifest"]})
            baseline = inventory(private)
            save(evidence / "private-before.json", baseline)
            if native:
                baseline_verify = process([validator, "--verify-package", private],
                                          private, evidence, "verify-before", options.timeout)
                require(baseline_verify["exitCode"] == 0 and not baseline_verify["timedOut"],
                        "Baseline native verification failed; root/input premise invalid")
            result = {"case": patch["case"], "status": "cpu-prepared",
                      "acceptancePassed": False}
            with patched(private, patch, evidence):
                current = inventory(private)
                changed = [name for name in baseline if baseline[name] != current.get(name)]
                require(changed == [patch["path"]] and current.keys() == baseline.keys(),
                        "Mutation changed more than the exact target")
                result["onlyExactTargetChanged"] = True
                if native:
                    install = process([validator, "--verify-package", private],
                                      private, evidence, "verify-mutated", options.timeout)
                    result["installationClassification"] = verify_rejection(install)
                    result["installationFailureLayer"] = installation_layer(install)
                    result["status"] = ("installation-rejection-observed" if
                                        result["installationClassification"] == "native-installation-rejected"
                                        else "failed")
                if mode == "execute":
                    argv = game_argv(private, config, options)
                    report["rendererStarted"] = True
                    save(output / "matrix.json", report)
                    environment = dict(os.environ)
                    quality = evidence / "quality"
                    quality.mkdir()
                    environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(quality)
                    save(evidence / "game-environment.json", {
                        "VULKANLEARN_RVT_QUALITY_DIR": str(quality)})
                    game = process(argv, private, evidence, "game", options.timeout, environment)
                    result.update(assess(patch["case"], game, install, patch))
            restored = inventory(private)
            result["privateInventoryRestored"] = restored == baseline
            require(restored == baseline, "Runtime changed installation closure; preserve copy for diagnosis")
            if native:
                verification = process([validator, "--verify-package", private],
                                       private, evidence, "verify-restored", options.timeout)
                require(verification["exitCode"] == 0 and not verification["timedOut"],
                        "Restored package rejected")
            report["results"].append(result)
            save(evidence / "result.json", result)
            save(output / "matrix.json", report)
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        after = inventory(package)
        report["sourceUnchanged"] = before == after
        save(output / "source-after.json", after)
        report["status"] = "failed" if report.get("failure") else mode + "-incomplete"
        save(output / "matrix.json", report)
        require(before == after, "Original package bytes changed")
    return 0 if mode == "prepare" else 2


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--private-root", type=Path,
                        help="Fresh short root for independent copies; source/output must not overlap")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--plan", action="store_true", help="Default; inspect and emit exact CPU patches only")
    modes.add_argument("--prepare", action="store_true", help="CPU copy/patch/restore; no child process")
    modes.add_argument("--verify-installations", action="store_true",
                       help="Copy/patch/native CPU installation verification/restore; never Game")
    modes.add_argument("--execute", action="store_true", help="Only in main's allocated GPU window")
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--worker", type=int, choices=(1, 2), default=2)
    from rvt_game_fault_runner import CASES
    parser.add_argument("--case", choices=tuple(dict.fromkeys((*FILE_CASES, *HOOKS, *CASES))), action="append")
    parser.add_argument("--capture-plan", type=Path,
                        help="Use the adaptive real-Game fault runner with actual Quality ABI identities")
    parser.add_argument("--package-local", type=Path, help="Optional second formal LocalLayerArray Game")
    parser.add_argument("--workers", nargs="+", type=int, choices=(1, 2),
                        help="Explicit serial worker matrix, normally 1 2")
    parser.add_argument("--binary-sha256", help="Actual final R9 Game executable digest, required for execution")
    parser.add_argument("--baseline-camera", default="far")
    parser.add_argument("--camera", default="tile00_near")
    parser.add_argument("--surface-id", default="terrain|Landscape_00")
    parser.add_argument("--mip", type=int, default=3)
    parser.add_argument("--page-x", type=int, default=0)
    parser.add_argument("--page-y", type=int, default=0)
    parser.add_argument("--world-generation", type=int)
    parser.add_argument("--revision", type=int)
    parser.add_argument("--replacement-revision", type=int)
    parser.add_argument("--status", type=int, default=1)
    parser.add_argument("--submit-result", type=int, default=-1)
    parser.add_argument("--admission-limit", type=int, default=1)
    parser.add_argument("--slot", type=int)
    parser.add_argument("--slot-publish-epoch", type=int)
    parser.add_argument("--producer-source", default="shader/glsl/terrain/terrainCompositedBakeWorld.comp")
    parser.add_argument("--phase-timeout", type=float, default=40)
    parser.add_argument("--frames", type=int, default=1400)
    parser.add_argument("--timeout", type=float, default=1800)
    options = parser.parse_args(argv)
    require(options.frames > 60 and options.timeout > 0, "Invalid execution bounds")
    try:
        if options.capture_plan:
            require(not options.prepare and not options.verify_installations,
                    "Adaptive Game runner uses --plan or --execute; legacy CPU mutation modes remain available separately")
            from rvt_game_fault_runner import run as run_game
            return run_game(options)
        return run(options)
    except (ValueError, OSError, KeyError, StopIteration, TypeError) as error:
        print(json.dumps({"status": "failed", "error": str(error), "acceptancePassed": False}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
