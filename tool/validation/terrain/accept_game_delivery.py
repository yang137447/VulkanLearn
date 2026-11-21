import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
from build_composited_fixture import V2_SCENE, published_world_files
from rvt_p0_analyze import extract
from rvt_delivery import IncompleteEvidence
from rvt_generation_identity import COOK_IDENTITY

ROOT = next(
    parent for parent in Path(__file__).resolve().parents
    if (parent / "CMakeLists.txt").is_file() and (parent / "config/config.json").is_file()
)
EVIDENCE = ROOT / "artifacts/terrain-composited-vt/game-delivery-final"
STAGE = ROOT.parent / "VulkanLearn-delivery/sf"
PRODUCT = ROOT.parent / "vt-game"
SCENE = V2_SCENE
EXPORTER = STAGE / "bin/vl_export_game.exe"
RESULTS = {}
TIMEOUT = 240
FRAMES = 180
NEGATIVE_CASES = (
    "corrupt-root", "corrupt-coarse", "corrupt-production-input", "missing-height", "missing-height-binary",
    "wrong-version", "wrong-filter-rule", "tr-resident-pages", "tr-height-budget",
    "tr-missing-composited-vt", "tr-disabled-composited-vt",
)


def negative_payload(name, original, offset=32):
    if name in ("corrupt-root", "corrupt-coarse", "corrupt-production-input"):
        changed = bytearray(original)
        if not 0 <= offset < len(changed):
            raise ValueError("Corruption offset outside payload")
        changed[offset] ^= 1
        return bytes(changed)
    if name in ("missing-height", "missing-height-binary"):
        return None
    changed = json.loads(original)
    if name == "wrong-filter-rule":
        changed["filterRule"] = "area-filter-local-normal-renormalized-v1"
    elif name == "tr-missing-composited-vt":
        del changed["compositedVT"]
    elif name == "tr-disabled-composited-vt":
        changed["compositedVT"]["enabled"] = False
    elif name == "wrong-version":
        changed["version"] = 1
    elif name == "tr-resident-pages":
        changed["compositedVT"]["residentPages"] = 192
    elif name == "tr-height-budget":
        changed["compositedVT"]["heightBudgetBytes"] = 8388608
    else:
        raise ValueError(f"Unknown negative case: {name}")
    return json.dumps(changed, ensure_ascii=False, indent=2).encode("utf-8")


def runtime_fault_policy(name):
    if name in ("corrupt-root", "corrupt-coarse"):
        return "rebuild-from-production"
    if name in NEGATIVE_CASES:
        return "reject-world"
    raise ValueError(f"Unknown fault policy: {name}")


def payload_corruption(record):
    if record.get("kind") not in ("root", "coarse") or record.get("reason") in (
            "absent", "not-packaged", "missing-root-record"):
        return False
    expected, observed = record.get("expectedDigest"), record.get("observedDigest")
    return (isinstance(expected, str) and isinstance(observed, str) and
            re.fullmatch("[0-9a-f]{64}", expected) is not None and
            re.fullmatch("[0-9a-f]{64}", observed) is not None and expected != observed)


def validate_recovery(text, page, root):
    events, errors = extract(text, "[RVT_EVIDENCE] ")
    if errors or not events:
        raise IncompleteEvidence("Recovery requires actual native root/loading/publication events")
    if not any(row.get("event") == "integrity" and payload_corruption(row) and
               all(row.get(field) == page[field] for field in ("surfaceId", "mip", "x", "y"))
               for row in events):
        raise IncompleteEvidence("No actual damaged payload hash evidence; legal absence is not corruption")
    publications = [(index, row) for index, row in enumerate(events)
                    if row.get("event") == "publication" and row.get("source") == "gpu"
                    and all(row.get(field) == page[field] for field in ("surfaceId", "mip", "x", "y"))
                    and row.get("status") == 0 and row.get("fenceCompletedEpoch", -1) >= row.get("writeEpoch", 0)]
    if not publications:
        raise IncompleteEvidence("Damaged packaged page was not observed rebuilt from valid production inputs")
    if root:
        loading = [index for index, row in enumerate(events) if row.get("event") == "loading"
                   and row.get("reason") == "root-not-ready"]
        ready = [index for index, row in enumerate(events) if row.get("event") == "root-ready"
                 and row.get("surfaceId") == page["surfaceId"]]
        samples = [row for row in events if row.get("event") == "quality"]
        if not loading or not ready or not min(loading) < publications[0][0] < max(ready) or not samples or any(
                row.get("uninitializedDraws", -1) != 0 for row in samples):
            raise IncompleteEvidence("Root rebuild did not prove loading before rootReady and initialized draws")
    return {"runtimePolicy": "rebuild-from-production", "rebuiltPage": page,
            "rootLoadingObserved": root, "nativeEvents": len(events)}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save():
    (EVIDENCE / "acceptance-results.json").write_text(
        json.dumps(RESULTS, indent=2), encoding="utf-8"
    )


def run(name, command, cwd, expected=0):
    command = [str(value) for value in command]
    print(name, flush=True)
    (EVIDENCE / f"{name}.command.json").write_text(
        json.dumps({"argv": command, "cwd": str(cwd)}, indent=2), encoding="utf-8"
    )
    with (EVIDENCE / f"{name}.log").open("wb") as output:
        process = subprocess.run(
            command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
            timeout=TIMEOUT, creationflags=subprocess.CREATE_NO_WINDOW,
        )
    text = (EVIDENCE / f"{name}.log").read_text(encoding="utf-8", errors="replace")
    RESULTS[name] = {"exitCode": process.returncode}
    (EVIDENCE / f"{name}.exit.txt").write_text(str(process.returncode))
    save()
    if expected == "nonzero":
        assert process.returncode != 0, f"{name} unexpectedly succeeded"
    else:
        assert process.returncode == expected, f"{name}: exit {process.returncode}"
    return text


def smoke(name, cwd, frames=120):
    executable = cwd / ("bin/main_game.exe" if cwd == STAGE else "main_game.exe")
    text = run(name, [
        executable, "--exit-after-frames", str(frames), "--worker-thread-count", "2",
        "--initial-scene", SCENE,
        "--no-dev-ui", "--console-command-delay", "60",
        "--console-command", "terrainvt stats",
        "--console-command", "terrainchunk stats",
        "--console-command", f"screenshot {(EVIDENCE / (name + '.bmp')).as_posix()}",
    ], cwd)
    assert "EngineLoop teardown complete" in text
    assert re.search(r"misses=0\b", text)
    assert not re.search(r"Shader cache miss:|Validation Error|VUID|THREADING|Scene.LoadFailed", text)
    RESULTS[name]["cleanTeardown"] = True
    RESULTS[name]["shaderMisses"] = 0
    save()


def main(options):
    RESULTS.update({
        "scene": SCENE, "package": str(PRODUCT), "stage": str(STAGE),
        "rendererAuthorizedByInvocation": options.execute,
        "status": "Prepared only",
        "negativeCasesRequested": options.negative_cases,
        "priorReceipt": str(options.prior_receipt.resolve()) if options.prior_receipt else None,
    })
    if options.prior_receipt:
        prior = json.loads(options.prior_receipt.read_text(encoding="utf-8-sig"))
        assert str(prior.get("status", "")).startswith("Package delivery checks passed"), "Prior receipt not passed"
        RESULTS["priorReceiptSha256"] = digest(options.prior_receipt)
    assert not Path(SCENE).is_absolute() and ".." not in Path(SCENE).parts
    assert PRODUCT != STAGE and STAGE not in PRODUCT.parents
    assert EXPORTER.is_file(), f"Missing exporter: {EXPORTER}"
    assert (STAGE / "bin/main_game.exe").is_file()
    config = json.loads((STAGE / "config/config.json").read_text(encoding="utf-8-sig"))
    resource_root = Path(config["resourcePath"])
    if not resource_root.is_absolute():
        resource_root = STAGE / resource_root
    assert (resource_root / SCENE).is_file(), "Source scene missing"
    RESULTS["sourceSceneSha256"] = digest(resource_root / SCENE)
    RESULTS["stageBinarySha256"] = digest(STAGE / "bin/main_game.exe")
    RESULTS["exporterSha256"] = digest(EXPORTER)
    scene = json.loads((resource_root / SCENE).read_text(encoding="utf-8-sig"))
    surfaces = []
    for placement in scene["objects"]:
        if placement["type"] != "terrain":
            continue
        terrain = json.loads((resource_root / placement["terrainPath"]).read_text(encoding="utf-8-sig"))
        assert terrain["compositedVT"]["virtualResolution"] == [
            options.expected_resolution, options.expected_resolution
        ], f"Unexpected virtual resolution: {placement['terrainPath']}"
        assert not {"residentPages", "heightBudgetBytes"} & terrain["compositedVT"].keys(), "Retired TR budgets"
        manifest_name = terrain["compositedVT"]["packagePath"]
        manifest_path = resource_root / manifest_name
        entry = {
            "terrain": placement["terrainPath"], "manifest": manifest_name,
            "virtualResolution": terrain["compositedVT"]["virtualResolution"],
        }
        if manifest_path.is_file():
            document = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
            assert document["version"] == 2, "Only World V2 is accepted"
            entry.update({
                "filterRule": document["filterRule"],
                "generatorVersions": [surface["generatorVersion"] for surface in document["surfaces"]],
                "encodings": [surface["encoding"] for surface in document["surfaces"]],
                "manifestSha256": digest(manifest_path),
                "pageCount": len(document["pages"]),
                "packageBytes": document["packageBytes"],
            })
            closure_names = [document["package"], *(item["path"] for item in document["production"]["objects"])]
            staging_prefix = Path(str(PRODUCT) + ".staging.999") / "resources"
            entry["longestOriginalMetadataPath"] = max(
                len(str(staging_prefix / Path(manifest_name).parent / name)) for name in closure_names
            )
            entry["flattenedMetadataPathUpperBound"] = max(
                len(str(staging_prefix / "VT" / ("0" * 16) / name)) for name in closure_names
            )
            entry["shortCookRelocationNeeded"] = entry["longestOriginalMetadataPath"] > 259
            surfaces.append(entry)
            RESULTS["sourceCookMetadata"] = surfaces
            save()
            if options.expected_filter_rule:
                assert entry["filterRule"] == options.expected_filter_rule, f"Stale filter: {manifest_path}"
            if options.expected_generator is not None:
                assert all(version == options.expected_generator for version in entry["generatorVersions"]), (
                    f"Stale generator: {manifest_path}"
                )
            if options.expected_encoding:
                assert all(encoding == options.expected_encoding for encoding in entry["encodings"]), (
                    f"Stale encoding: {manifest_path}"
                )
        else:
            entry["missing"] = True
            if options.expected_filter_rule or options.expected_generator is not None or options.expected_encoding or options.execute:
                raise RuntimeError(f"Cook manifest missing: {manifest_path}")
        if entry not in surfaces:
            surfaces.append(entry)
    RESULTS["sourceCookMetadata"] = surfaces
    save()
    if not options.execute:
        print("Offline preflight only. No renderer, Cook, export or package mutation.", flush=True)
        return
    RESULTS["sharedCacheBefore"] = digest(ROOT / "shader/spv/shader-build-cache.json")
    if options.reuse_package:
        assert PRODUCT.is_dir(), "Requested existing package missing"
        package_config = json.loads((PRODUCT / "config/config.json").read_text(encoding="utf-8"))
        assert package_config["initScene"] == SCENE, "Existing package scene mismatch"
    else:
        if PRODUCT.exists():
            raise RuntimeError(f"Existing product requires inspection before reuse: {PRODUCT}")
        if options.stage_warm_receipt:
            warm = json.loads(options.stage_warm_receipt.read_text(encoding="utf-8-sig"))
            assert warm["status"] == "passed" and warm["scene"] == SCENE
            assert warm["stageBinarySha256"] == digest(STAGE / "bin/main_game.exe")
            assert warm["shaderCacheSha256"] == digest(STAGE / "shader/spv/shader-build-cache.json")
            assert warm["exitCode"] == 0 and warm["noShaderCompile"] and warm["cleanTeardown"]
            assert Path(warm["runtimeLog"]).is_file() and digest(Path(warm["runtimeLog"])) == warm["runtimeLogSha256"]
            RESULTS["stage-warm"] = {"reusedReceipt": str(options.stage_warm_receipt.resolve()),
                                     "receiptSha256": digest(options.stage_warm_receipt), "exitCode": 0}
            save()
        else:
            smoke("stage-warm", STAGE)
        run("export", [
            EXPORTER, "--project-root", STAGE, "--binary-directory", STAGE / "bin",
            "--initial-scene", SCENE, "--output", PRODUCT, "--flavor", "game",
        ], STAGE)
    run("verify-pristine", [EXPORTER, "--verify-package", PRODUCT], PRODUCT)
    smoke("package-cold", PRODUCT, FRAMES)
    resources = PRODUCT / "resources"
    scene = json.loads((resources / SCENE).read_text(encoding="utf-8"))
    placements = [entry for entry in scene["objects"] if entry["type"] == "terrain"]
    terrain_path = resources / placements[0]["terrainPath"]
    terrain = json.loads(terrain_path.read_text(encoding="utf-8"))
    manifest = resources / terrain["compositedVT"]["packagePath"]
    document = json.loads(manifest.read_text(encoding="utf-8"))
    binary = manifest.parent / document["package"]
    root_keys = {tuple(root[field] for field in ("surfaceId", "mip", "x", "y")) for root in document["roots"]}
    root_page = next(page for page in document["pages"]
                     if tuple(page[field] for field in ("surfaceId", "mip", "x", "y")) in root_keys)
    coarse_page = next((page for page in document["pages"]
                        if tuple(page[field] for field in ("surfaceId", "mip", "x", "y")) not in root_keys), None)
    input_id = document["production"]["surfaces"][0]["inputs"]["layerAlbedo"]
    input_object = next(entry for entry in document["production"]["objects"] if entry["objectId"] == input_id)
    input_binary = manifest.parent / input_object["path"]
    height_ids = {entry["inputs"]["heightAtlas"] for entry in document["production"]["surfaces"]}
    height_objects = [entry for entry in document["production"]["objects"]
                      if entry["objectId"] in height_ids]
    assert height_objects, "Missing independent height production object"
    height_binary = manifest.parent / height_objects[0]["path"]
    RESULTS["closure"] = {
        "surfaces": len(placements),
        "compositedManifests": len(list(resources.rglob("*.vlcvt"))),
        "heightObjects": len(height_objects),
        "packageResourcePath": json.loads((PRODUCT / "config/config.json").read_text(encoding="utf-8"))["resourcePath"],
    }
    assert len(placements) == options.expected_surfaces, "Unexpected terrain placement count"
    assert RESULTS["closure"]["packageResourcePath"] == "resources"
    negative_paths = dict([
        ("corrupt-root", binary), ("corrupt-coarse", binary), ("corrupt-production-input", input_binary),
        ("missing-height", height_binary), ("missing-height-binary", height_binary),
        ("wrong-version", manifest),
        ("wrong-filter-rule", manifest),
        ("tr-missing-composited-vt", terrain_path), ("tr-disabled-composited-vt", terrain_path),
        ("tr-resident-pages", terrain_path), ("tr-height-budget", terrain_path),
    ])
    RESULTS["negativeCasesExecuted"] = []
    RESULTS["negativeCasesNotApplicable"] = {}
    for name in options.negative_cases:
        if name == "corrupt-coarse" and coarse_page is None:
            RESULTS["negativeCasesNotApplicable"][name] = "Publication contains roots only; no packaged coarse payload"
            save()
            continue
        path = negative_paths[name]
        original = path.read_bytes()
        backup = EVIDENCE / f"{name}.original"
        backup.write_bytes(original)
        try:
            selected_page = root_page if name == "corrupt-root" else coarse_page
            offset = selected_page["offset"] if name in ("corrupt-root", "corrupt-coarse") else 0
            changed = negative_payload(name, original, offset)
            if changed is None:
                path.unlink()
            else:
                path.write_bytes(changed)
            verification = run(f"{name}-verify", [
                EXPORTER, "--verify-package", PRODUCT,
            ], PRODUCT, "nonzero")
            assert re.search(r"error|mismatch|missing|resolve|not found|invalid", verification, re.I)
            for workers in (1, 2):
                label = f"{name}-runtime-w{workers}"
                recovery = runtime_fault_policy(name) == "rebuild-from-production"
                text = run(label, [
                    PRODUCT / "main_game.exe", "--exit-after-frames", str(FRAMES if recovery else 2), "--no-dev-ui",
                    "--worker-thread-count", str(workers),
                ], PRODUCT, 0 if recovery else "nonzero")
                if recovery:
                    assert "EngineLoop teardown complete" in text
                    assert not re.search(r"Validation Error|VUID|THREADING|EngineLoop init failed", text)
                    RESULTS[label].update(validate_recovery(text, selected_page, name == "corrupt-root"))
                    save()
                    continue
                assert RESULTS[label]["exitCode"] == 1, "Crash is not an accepted load failure"
                assert "EngineLoop init failed:" in text, "Missing explicit initialization failure"
                assert re.search(r"terrain|height|composited|vlcvt", text, re.I)
                assert "EngineLoop teardown complete" in text, "Failure did not cleanly tear down"
                assert not re.search(r"Validation Error|VUID|THREADING", text)
                assert "World/graph transaction committed:" not in text, "Corrupt input/config published a World"
                if name.startswith("tr-"):
                    assert re.search(r"schema|validation|compositedVT|enabled", text, re.I), "Missing cutover rejection diagnostic"
                    assert "World/graph transaction committed:" not in text, "Rejected TR published a fallback World"
        finally:
            path.write_bytes(original)
            assert digest(path) == hashlib.sha256(original).hexdigest()
            RESULTS[f"{name}-restored"] = True
            save()
        run(f"{name}-verify-restored", [EXPORTER, "--verify-package", PRODUCT], PRODUCT)
        RESULTS["negativeCasesExecuted"].append(name)
        save()
    run("verify-restored", [EXPORTER, "--verify-package", PRODUCT], PRODUCT)
    run("official-validator", [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        ROOT / "tool/validate-game-package.ps1", "-PackagePath", PRODUCT,
        "-VerifyTool", EXPORTER, "-SmokeFrames", str(FRAMES),
    ], ROOT)
    RESULTS["sharedCacheAfter"] = digest(ROOT / "shader/spv/shader-build-cache.json")
    assert RESULTS["sharedCacheBefore"] == RESULTS["sharedCacheAfter"]
    RESULTS["status"] = "Package checks collected; inspect executed and not-applicable scope; P5 not certified"
    RESULTS["acceptancePassed"] = False
    save()


def cli():
    global ROOT, SCENE, PRODUCT, STAGE, EXPORTER, EVIDENCE, TIMEOUT, FRAMES
    parser = argparse.ArgumentParser(
        description="Offline preflight by default; --execute runs actual game package acceptance. Never runs Cook."
    )
    parser.add_argument("--scene", default=SCENE)
    parser.add_argument("--project-root", type=Path, default=ROOT)
    parser.add_argument("--packagepath", "--package-path", type=Path, default=PRODUCT)
    parser.add_argument("--stage", type=Path, default=STAGE)
    parser.add_argument("--exporter", type=Path)
    parser.add_argument("--evidence-root", type=Path, default=EVIDENCE)
    parser.add_argument("--run-id", "--runid", default=datetime.now().strftime("%Y%m%d-%H%M%S"))
    parser.add_argument("--expected-filter-rule", default=COOK_IDENTITY["filterRule"])
    parser.add_argument("--expected-generator", type=int, default=COOK_IDENTITY["generatorVersion"])
    parser.add_argument("--expected-encoding", default=COOK_IDENTITY["encoding"])
    parser.add_argument("--expected-resolution", type=int, default=8192)
    parser.add_argument("--expected-surfaces", type=int, default=16)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--frames", type=int, default=180)
    parser.add_argument("--reuse-package", action="store_true")
    parser.add_argument("--prior-receipt", type=Path)
    parser.add_argument("--stage-warm-receipt", type=Path,
                        help="Reuse matching successful current stage runtime instead of another Full initialization")
    parser.add_argument("--negative-cases", nargs="+", choices=NEGATIVE_CASES, default=list(NEGATIVE_CASES))
    parser.add_argument("--execute", action="store_true", help="Run renderer and mutate test payloads; requires renderer authorization")
    options = parser.parse_args()
    if set(options.negative_cases) != set(NEGATIVE_CASES):
        assert options.prior_receipt and options.prior_receipt.is_file(), "Selected negative rerun requires prior receipt"
    assert len(set(options.negative_cases)) == len(options.negative_cases), "Duplicate negative case"
    ROOT = options.project_root.resolve()
    assert (ROOT / "config/config.json").is_file(), "Invalid source project root"
    SCENE = options.scene.replace("\\", "/")
    PRODUCT = options.packagepath.resolve()
    STAGE = options.stage.resolve()
    EXPORTER = options.exporter.resolve() if options.exporter else STAGE / "bin/vl_export_game.exe"
    TIMEOUT = options.timeout
    FRAMES = options.frames
    assert TIMEOUT > 0 and FRAMES >= 90
    assert re.fullmatch(r"[A-Za-z0-9_-]+", options.run_id), "Unsafe run id"
    EVIDENCE = options.evidence_root.resolve() / options.run_id
    EVIDENCE.mkdir(parents=True, exist_ok=False)
    try:
        main(options)
    except Exception as error:
        RESULTS["error"] = str(error)
        save()
        raise


if __name__ == "__main__":
    cli()
