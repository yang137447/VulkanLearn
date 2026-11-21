"""Full private diagnostic stage and optional serial before/after capture, no signed-package edits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "artifacts/terrain-rvt-production/geometry-seam-fix"
BEFORE = ROOT.parent / "p5g/tourr4"
AFTER = ROOT.parent / "p5g/ga1"
SOURCE = Path("shader/glsl/materialFunction/mf_terrainLayerBlend.glsl")
SCENE = "Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {path.relative_to(root).as_posix(): {"bytes": path.stat().st_size, "sha256": digest(path)}
            for path in root.rglob("*") if path.is_file()}


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def environment(stage):
    env = os.environ.copy()
    env["PATH"] = str(stage) + ";" + env["PATH"]
    return env


def prepare():
    baseline = inventory(BEFORE)
    if AFTER.exists():
        raise RuntimeError("Private ga1 already exists; inspect/reuse its receipt, never overwrite")
    shutil.copytree(BEFORE, AFTER)
    shutil.copy2(ROOT / SOURCE, AFTER / SOURCE)
    command = [str(OUT / "compiler.exe"), str(AFTER),
               str(BEFORE / "resources/VT/775359b06fde29e5/manifest.json"),
               str(AFTER / SOURCE), "--base-only"]
    result = subprocess.run(command, env=environment(AFTER), cwd=ROOT, capture_output=True, text=True)
    (OUT / "full-stage-compile.log").write_text(repr(command)+"\n"+result.stdout+result.stderr, encoding="utf-8")
    result.check_returncode()
    staged = inventory(AFTER)
    deltas = [{"path": name, "before": baseline.get(name), "after": staged.get(name)}
              for name in sorted(set(baseline) | set(staged)) if baseline.get(name) != staged.get(name)]
    allowed = lambda name: (name == SOURCE.as_posix() or name == "shader/spv/shader-build-cache.json"
                            or name.startswith(("shader/spv/material/", "geometry-diagnostic/", "link-inputs/Base"))
                            or name.startswith(("shader/spv/pass/terrainFeedback/", "shader/spv/pass/terrainFeedback_")))
    assert all(allowed(item["path"]) for item in deltas), deltas
    original_cache = json.loads((BEFORE / "shader/spv/shader-build-cache.json").read_text())
    after_cache = json.loads((AFTER / "shader/spv/shader-build-cache.json").read_text())
    kernels = []
    for key, record in original_cache["artifacts"].items():
        if "terrainCompositedBakeWorld" in record["normalizedKey"]:
            assert record == after_cache["artifacts"][key]
            kernels.append(record)
    assert len(kernels) == 2
    assert baseline == inventory(BEFORE)
    assert digest(BEFORE / "main_game.exe") == digest(AFTER / "main_game.exe")
    report = {"scope": "Private complete diagnostic copy; original export manifest does not certify changed shaders/cache",
              "gpuStarted": False, "beforeStage": str(BEFORE), "afterStage": str(AFTER),
              "mainGameSha256": digest(AFTER / "main_game.exe"),
              "resourcesBinConfigAndPublicationBytesUnchanged": True,
              "oldSignedPackageUnchanged": True, "approvedDelta": deltas,
              "kernelRecordsUnchanged": kernels,
              "publicationScope": "Original raw-author archived bytes/full production hash retained as diagnostic baseline. No author validation bypass or manifest hash rewriting. Actual Game consumption must be established by runtime logs; Editor/formal export refresh remains required.",
              "command": command}
    save(OUT / "full-stage-prepared.json", report)
    save(OUT / "full-stage-before-inventory.json", baseline)
    save(OUT / "full-stage-after-inventory.json", staged)
    return report


def run_shot(stage, label, output, await_quality=False, recover_saved=False):
    output.mkdir(parents=True, exist_ok=recover_saved)
    screenshot = output / "junction_0_0.bmp"
    camera = "camera pose -100800 9723.415039 -99600 -100800 8923.415039 -100800"
    commands = [camera] + ["camera get"] * 120 + ["terrainvt stats", "terrainchunk stats",
                 "terrainlod stats", "camera get", "screenshot " + screenshot.as_posix()]
    commands += ["camera get"] * 30 + ["terrainchunk stats", "terrainlod stats"]
    if await_quality:
        commands = [camera]
    argv = [str(stage / "main_game.exe"), "--initial-scene", SCENE,
            "--worker-thread-count", "2", "--frame-limit", "30",
            "--present-mode", "immediate", "--no-dev-ui", "--validation",
            "--console-command-delay", "120", "--exit-after-frames",
            "600" if await_quality else str(180+len(commands))]
    for command in commands:
        argv += ["--console-command", command]
    if not recover_saved:
        save(output / "command.json", {"stage": str(stage), "label": label, "argv": argv})
    env = environment(stage)
    env["VULKANLEARN_RUNTIME_CACHE_ROOT"] = str(output / "cache")
    if recover_saved:
        text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
        assert "EngineLoop teardown complete." in text
        assert "Screenshot saved: " + screenshot.as_posix() in text.replace("\\", "/")
        assert (output / "capture-await-quality.json").is_file()
        code = 0
    else:
      with (output / "runtime.log").open("wb") as log:
        process = subprocess.Popen(argv, cwd=stage, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        save(output / "process.json", {"pid": process.pid, "started": time.time()})
        try:
            if await_quality:
                deadline = time.monotonic() + 150
                selected = None
                while time.monotonic() < deadline:
                    text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
                    states = [json.loads(line.split("[RVT_QUALITY_STATE] ", 1)[1])
                              for line in text.splitlines() if line.startswith("[RVT_QUALITY_STATE] ")]
                    valid = [state for state in states if state["poseId"] == 2 and state["completedEpoch"] >= 150
                             and state["visible"] > 0 and state["validAtRawTargetPlusOne"] / state["visible"] >= .99
                             and state["interactive"] and not state["convergenceFailed"]
                             and state["incorrectIdentity"] == 0 and state["unresolved"] == 0]
                    if valid and "position=(-100800.000000, 9723.415039, -99600.000000)" in text:
                        selected = valid[-1]
                        break
                    if process.poll() is not None:
                        raise RuntimeError("Renderer ended before same-pose quality readiness")
                    time.sleep(.25)
                if selected is None:
                    raise RuntimeError("Same-pose quality did not converge; no screenshot accepted")
                save(output / "capture-await-quality.json", {
                    "pose": "junction_0_0", "qualityState": selected,
                    "scope": "same static pose after settled; screenshot not claimed exact quality epoch"})
                def inject(command):
                    argv = ["pwsh", "-NoProfile", "-File",
                            str(ROOT / "tool/validation/console_inject.ps1"),
                            "-InjectProcessId", str(process.pid), "-InjectLine", command]
                    result = subprocess.run(argv, capture_output=True, timeout=8,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                    with (output / "injection.jsonl").open("a", encoding="utf-8") as stream:
                        stream.write(json.dumps({"command": command, "exitCode": result.returncode,
                                                 "atMonotonic": time.monotonic()}) + "\n")
                    result.check_returncode()
                for command in ("camera get", "terrainvt stats", "terrainchunk stats",
                                "terrainlod stats", "screenshot " + screenshot.as_posix()):
                    inject(command)
                while time.monotonic() < deadline:
                    text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
                    if screenshot.is_file() and "Screenshot saved: " + screenshot.as_posix() in text.replace("\\", "/"):
                        data = screenshot.read_bytes()
                        if data[:2] == b"BM" and struct.unpack_from("<I", data, 2)[0] == len(data):
                            break
                    if process.poll() is not None:
                        raise RuntimeError("Renderer ended before complete BMP save")
                    time.sleep(.25)
                else:
                    raise RuntimeError("Screenshot did not complete before diagnostic deadline")
                for command in ("terrainchunk stats", "terrainlod stats", "quit"):
                    inject(command)
            code = process.wait(timeout=180)
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            raise
    text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
    report = {"label": label, "exitCode": code, "screenshotExists": screenshot.is_file(),
              "camera": [line for line in text.splitlines() if "Camera pose set:" in line],
              "lodStats": [line for line in text.splitlines() if "[TerrainChunk]" in line or "TerrainLOD" in line],
              "errors": [line for line in text.splitlines() if "VUID-" in line or "init failed" in line or "World load failed" in line],
              "worldPositionDepthReadback": False, "normalTessDisplacementUnchanged": True,
              "scope": "RGB effect and aggregate chunk/LOD only; no per-pixel depth/ID proof"}
    if screenshot.is_file():
        report["screenshotSha256"] = digest(screenshot)
        data = screenshot.read_bytes()
        report["bmp"] = {"signature": data[:2].decode("ascii"), "actualLength": len(data),
                         "declaredLength": struct.unpack_from("<I", data, 2)[0],
                         "pixelOffset": struct.unpack_from("<I", data, 10)[0],
                         "width": struct.unpack_from("<i", data, 18)[0],
                         "height": struct.unpack_from("<i", data, 22)[0],
                         "bitsPerPixel": struct.unpack_from("<H", data, 28)[0]}
    report["samePoseQualityAwaited"] = await_quality
    if recover_saved:
        report["harnessIssue"] = "Initial saved-path comparison used slash spelling; BMP was already complete. Recovered existing screenshot after native 600-frame clean shutdown, no second before render."
    report["shaderCacheLog"] = [line for line in text.splitlines()
                               if "Shader build:" in line or "Shader cache miss:" in line]
    save(output / "result.json", report)
    if code or not screenshot.is_file() or report["errors"]:
        raise RuntimeError("Diagnostic shot failed; inspect " + str(output))
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--prepare-before", action="store_true")
    parser.add_argument("--await-quality", action="store_true")
    parser.add_argument("--recover-before", action="store_true")
    parser.add_argument("--output", type=Path, default=OUT / "ab-01")
    args = parser.parse_args()
    if args.prepare:
        report = prepare()
        print(json.dumps({"prepared": report["afterStage"], "approvedDeltaCount": len(report["approvedDelta"]), "gpuStarted": False}))
    before_copy = ROOT.parent / "p5g/gb1"
    if args.prepare_before or args.execute:
        baseline = json.loads((OUT / "full-stage-before-inventory.json").read_text(encoding="utf-8"))
        assert inventory(BEFORE) == baseline, "Original source stage changed"
        if not before_copy.exists():
            shutil.copytree(BEFORE, before_copy)
        assert inventory(before_copy) == baseline, "Private before copy differs from original"
        save(OUT / "before-stage-prepared.json", {"stage": str(before_copy), "sourceUnchanged": True,
              "allFilesIdentical": True, "gpuStarted": False})
    if args.execute:
        receipt = json.loads((OUT / "full-stage-prepared.json").read_text(encoding="utf-8"))
        assert receipt["afterStage"] == str(AFTER)
        expected = json.loads((OUT / "full-stage-after-inventory.json").read_text(encoding="utf-8"))
        assert inventory(AFTER) == expected, "Prepared stage has changed"
        output = args.output.resolve()
        reports = [run_shot(stage, label, output / label, args.await_quality, args.recover_before and label == "before")
                   for stage, label in ((before_copy, "before"), (AFTER, "after"))]
        save(output / "result.json", {"shots": reports, "visualAcceptancePassed": False,
             "originalStageUnchanged": inventory(BEFORE) == baseline})
    if not args.prepare and not args.execute and not args.prepare_before:
        print("Use --prepare for CPU copy/cache work; --execute for two serial GPU shots.")


if __name__ == "__main__":
    main()
