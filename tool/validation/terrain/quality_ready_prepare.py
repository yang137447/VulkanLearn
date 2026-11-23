"""Freeze the proven R3 native objects and bind the pure Quality ABI v2 explicitly."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section_hashes(path):
    data = path.read_bytes()
    sections = struct.unpack_from("<H", data, 2)[0]
    optional = struct.unpack_from("<H", data, 16)[0]
    return [hashlib.sha256(data[offset:offset + size]).hexdigest()
            for size, offset in (struct.unpack_from("<II", data, 20 + optional + index * 40 + 16)
                                 for index in range(sections))]


def main():
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-quality-ready"
    output.mkdir(parents=True, exist_ok=True)
    winner = root / "artifacts/terrain-rvt-production/p3-quality-winner"
    base = json.loads((winner / "prepared.json").read_text("utf-8"))
    if digest(winner / "stage/bin/main_game.exe") != base["executableSha256"]:
        raise RuntimeError("Proven winner stage executable changed")
    stage = output / "stage"
    if not stage.exists():
        shutil.copytree(winner / "stage", stage)
    native = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality/run006-coherent"
    handoff_file = root / "artifacts/terrain-rvt-production/r2-clock-metadata-mi-r3-service-handoff.json"
    handoff = json.loads(handoff_file.read_text("utf-8"))
    header = Path(os.environ["TEMP"]) / "vl-rvt-p2-budget-bounded/terrainRvtBudget.h"
    expected_budget = next(value for name, value in handoff["sourcesAfter"].items()
                           if Path(name).name == "terrainRvtBudget.h")
    if digest(header) != expected_budget:
        raise RuntimeError("R3 Budget header differs from locked ABI")
    overlay = output / "locked-headers/terrain"
    overlay.mkdir(parents=True, exist_ok=True)
    shutil.copy2(header, overlay / header.name)
    commands = []

    def run(command, label, environment=None):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run(command, cwd=root, env=environment, capture_output=True, text=True)
        (output / (label + ".log")).write_text(
            repr(command) + "\n" + result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            print(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    source_paths = ["source/render/frame/terrainCompositedQualityFrame.cpp",
                    "tool/validation/terrain/quality_r3_recompute_v2.cpp"]
    hashes = {name: digest(root / name) for name in source_paths}
    objects = []
    closures = {}
    for name in source_paths:
        target = output / (Path(name).stem + ".obj")
        preprocessed = target.with_suffix(".ii")
        flags = ["-std=gnu++17", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
                 "-ffunction-sections", "-fdata-sections",
                 "-I" + str(overlay.parent), "@" + str(native / "includes.rsp")]
        run(["g++", *flags, "-E", root / name, "-o", preprocessed], Path(name).stem + "-preprocess")
        paths = {Path(path) for path in re.findall(r'^# \d+ "([^"]+)"',
                 preprocessed.read_text("utf-8", errors="replace"), re.MULTILINE)
                 if not path.startswith("<")}
        paths = {path if path.is_absolute() else root / path for path in paths}
        closures[name] = {str(path.resolve()): digest(path) for path in paths if path.is_file()}
        run(["g++", "-std=gnu++17", "-O0", "-ffunction-sections", "-fdata-sections",
             "-c", preprocessed, "-o", target], Path(name).stem + "-compile")
        objects.append(target)
    symbol = "_ZN2VL17TerrainQualityAbi9RecomputeEPKjy"
    originals = [Path(handoff["requiredObjects"][0]["path"]),
                 winner / "terrainCompositedQualityGpu.obj"]
    bound = []
    binding_receipts = []
    for original in originals:
        private = output / (original.stem + "-v2-binding.obj")
        run(["objcopy", "--weaken-symbol=" + symbol, original, private], original.stem + "-binding")
        if section_hashes(original) != section_hashes(private):
            raise RuntimeError("Symbol binding unexpectedly changed object section contents")
        bound.append(private)
        binding_receipts.append({"input": str(original), "inputSha256": digest(original),
            "output": str(private), "outputSha256": digest(private),
            "operation": "weaken only Recompute so the newly compiled strong ABI v2 definition owns all calls",
            "allSectionContentsByteIdentical": True})
    snapshots = Path(handoff["requiredObjects"][1]["path"])
    for item in handoff["requiredObjects"]:
        if digest(Path(item["path"])) != item["sha256"]:
            raise RuntimeError("Locked R3 native object changed")
    run(["g++", "-std=gnu++17", "-O0", root / "tool/validation/terrain/quality_r3_abi_probe.cpp",
         objects[1], bound[0], snapshots, "-Wl,--gc-sections",
         "-o", output / "r3-v2-probe.exe", "@" + str(native / "libraries.rsp")], "r3-v2-probe-link")
    environment = os.environ.copy()
    environment["PATH"] = str(stage / "bin") + ";" + environment["PATH"]
    run([output / "r3-v2-probe.exe", "v2"], "r3-v2-probe-run", environment)
    frame_recorder = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality/feedback-clock-r3/renderFrameRecorder.obj"
    fixed = [winner / (name + ".obj") for name in (
        "main_game", "renderGraphCompiler", "renderGraphCompilerValidation",
        "renderGraphFormats", "renderGraphPassResources")]
    run(["g++", *objects, *bound, *fixed, frame_recorder, snapshots,
         "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "game.map"),
         "-o", stage / "bin/main_game.exe", "@" + str(native / "libraries.rsp")], "game-link")
    if any(digest(root / name) != expected for name, expected in hashes.items()):
        raise RuntimeError("Quality source changed during the frozen CPU compilation")
    if any(digest(Path(name)) != expected for closure in closures.values()
           for name, expected in closure.items()):
        raise RuntimeError("A dependency changed after preprocessing; rebuild from a stable closure")
    manifest = {
        "gpuExecuted": False, "moduleOnly": False,
        "scope": "Proven R3 Service/SourceSnapshots machine code retained; only Recompute symbol binding replaced by a newly compiled pure QualityData v2 function. Service source was not rebuilt.",
        "r3SourceSnapshotAvailable": False, "currentHalfCowServiceCompiled": False,
        "winnerBaseSha256": base["executableSha256"],
        "r3HandoffSha256": digest(handoff_file), "bindingReceipts": binding_receipts,
        "r3SourceSnapshotsSha256": digest(snapshots), "r3BudgetHeaderSha256": digest(header),
        "freshSourceSha256": hashes, "compileClosures": closures,
        "frozenPreprocessedSha256": {str(path): digest(path) for path in output.glob("*.ii")},
        "overlayObjectSha256": {str(path): digest(path) for path in objects + bound + fixed +
                               [frame_recorder, snapshots]},
        "supportArchiveSha256": digest(native / "runtime-support-run006.a"),
        "executable": str(stage / "bin/main_game.exe"),
        "executableSha256": digest(stage / "bin/main_game.exe"),
        "stageConfigSha256": digest(stage / "config/config.json"),
        "stageGraphSha256": digest(stage / "config/renderGraphConfig.json"),
        "resourceRoot": json.loads((stage / "config/config.json").read_text("utf-8"))["resourcePath"],
        "abiProbePassed": True, "rawBeforeObserve": True, "commands": commands}
    (output / "prepared.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("Prepared non-module-only R3/v2 Game: " + str(output / "prepared.json"), flush=True)


if __name__ == "__main__":
    main()
