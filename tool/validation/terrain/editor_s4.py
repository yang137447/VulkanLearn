"""Build/run the S4 host adapter, or inspect its evidence without a renderer."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import struct
import sys
import tempfile

from run_composited_acceptance import (
    bmp_integrity, digest, inside, read_json, stage_project, terrain_stats, write_json, run_process,
)
from editor_s4_prepare import DEFAULT_SCENE, prepare

STAGES = ("baseline", "paint", "paint_undo", "paint_redo", "sculpt",
          "sculpt_undo", "sculpt_redo", "rapid", "material", "material_revert",
          "material_reject", "conflict", "conflict_recover", "save")
PROJECT = Path(__file__).resolve().parents[3]


def editor_stats(text, expected_surfaces):
    blocks, problems = terrain_stats(text, expected_surfaces)
    if blocks and all(int(surface.get("stale", -1)) == 0 and
                      int(surface.get("queued", -1)) == 0 and
                      int(surface.get("reading", -1)) == 0
                      for surface in blocks[-1]["surfaces"]):
        problems = [problem for problem in problems if not problem.startswith("Final dirty did not settle:")]
    return blocks, problems


def build(build_root, destination):
    build_root = build_root.resolve(strict=True)
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    cache = (build_root / "CMakeCache.txt").read_text(encoding="utf-8")
    compiler = re.search(r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache, re.M).group(1)
    flags = (build_root / "source/CMakeFiles/main_editor.dir/flags.make").read_text()
    definitions = re.search(r"^CXX_DEFINES = (.*)$", flags, re.M).group(1).split()
    compile_flags = shlex.split(re.search(r"^CXX_FLAGS = (.*)$", flags, re.M).group(1))
    includes = build_root / "source/CMakeFiles/main_editor.dir/includes_CXX.rsp"
    libraries = build_root / "source/CMakeFiles/main_editor.dir/linkLibs.rsp"
    source = Path(__file__).with_name("editor_s4_main.cpp")
    obj = destination / "editor_s4_main.o"
    exe = destination / "editor_s4.exe"
    commands = [
        [compiler, *definitions, "@" + str(includes), *compile_flags,
         "-c", str(source), "-o", str(obj)],
        [compiler, str(obj), "-Wl,--strip-debug", "-o", str(exe), "@" + str(libraries)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, cwd=build_root / "source", timeout=240,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (destination / f"build-{index}.log").write_bytes(result.stdout)
        if result.returncode:
            raise RuntimeError(f"Build failed; see {destination / f'build-{index}.log'}")
    write_json(destination / "build.json", {
        "commands": commands, "sourceSha256": digest(source), "exeSha256": digest(exe),
        "librariesResponseSha256": digest(libraries),
        "note": "Links existing main_editor libraries; rebuild them before acceptance.",
    })
    return exe


def analyze(directory, placement, report_name="editor-s4-report.json"):
    if Path(report_name).name != report_name or Path(report_name).suffix != ".json":
        raise ValueError("Report name must be a local JSON filename")
    text = (directory / "editor.log").read_text(encoding="utf-8", errors="replace")
    failures, missing = [], []
    states, histories, obsolete, working_set = {}, {}, {}, {}
    for stage in STAGES:
        match = re.search(rf"EDITOR_S4_STAGE {stage}\r?\n(.*?)(?=EDITOR_S4_STAGE |\Z)",
                          text, re.S)
        if not match:
            missing.append(f"{stage}: no stage")
            continue
        section = match.group(1)
        obsolete[stage] = [int(value) for value in re.findall(r"staleBakeResults=(\d+)", section)]
        rows = re.findall(r"EDITOR_S4_STATE (\{[^\r\n]+\})", section)
        if rows:
            states[stage] = json.loads(rows[-1])
        else:
            missing.append(f"{stage}: no World snapshot")
        count = re.search(r"TerrainVT generation=\d+ surfaces=(\d+)", section)
        blocks, problems = editor_stats(section, int(count.group(1)) if count else 1)
        failures.extend(f"{stage}: {problem}" for problem in problems)
        target = [surface for block in blocks for surface in block["surfaces"]
                  if surface["surfaceId"] == placement]
        if not target:
            missing.append(f"{stage}: no target placement statistics")
        else:
            histories[stage] = target
        if blocks:
            header = blocks[-1]["header"]
            working_set[stage] = {
                "previewReadbacks": int(header.get("previewReadbacks", -1)),
                "surfaces": [{"surfaceId": row["surfaceId"],
                              **{name: int(row.get(name, -1)) for name in ("stale", "queued", "reading")}}
                             for row in blocks[-1]["surfaces"]],
            }
            if int(header.get("previewReadbacks", -1)) != 0:
                failures.append(f"{stage}: GPU preview readbacks did not settle")
            for key in ("pendingEditorUpdates", "pendingMaterialUpdates"):
                values = re.findall(rf"\b{key}=(\d+)", section)
                working_set[stage][key] = int(values[-1]) if values else -1
                if not values or int(values[-1]):
                    failures.append(f"{stage}: {key} did not settle")

    def equal(first, second, field, expected=True):
        if first in states and second in states:
            if (states[first][field] == states[second][field]) != expected:
                failures.append(f"{field}: {first} vs {second}, expected equal={expected}")

    for field, first, edited, undone, redone in (
        ("paint", "baseline", "paint", "paint_undo", "paint_redo"),
        ("height", "paint_redo", "sculpt", "sculpt_undo", "sculpt_redo"),
    ):
        equal(first, edited, field, False)
        equal(first, undone, field)
        equal(edited, redone, field)
    equal("paint_redo", "sculpt_redo", "paint")
    equal("baseline", "paint_redo", "height")
    equal("sculpt_redo", "rapid", "paint", False)
    equal("sculpt_redo", "rapid", "height")
    for field in ("height", "paint"):
        equal("rapid", "save", field)
        for stage in STAGES[8:]:
            equal("rapid", stage, field)
    for stage in ("material", "material_revert", "material_reject", "conflict_recover", "save"):
        equal("baseline", stage, "materialDiskHash")
    equal("baseline", "conflict", "materialDiskHash", False)
    for stage in ("material_revert", "material_reject", "conflict", "conflict_recover", "save"):
        equal("baseline", stage, "liveUv")
    if "baseline" in states and "material" in states:
        if abs(states["material"]["liveUv"] - states["baseline"]["liveUv"] * 1.75) > 1e-5:
            failures.append("Numeric preview did not apply u_uvScale to the live World MI")
    previews = {row["stage"]: row for row in (
        json.loads(line) for line in re.findall(r"EDITOR_S4_PREVIEW (\{[^\r\n]+\})", text))}
    for stage in ("material", "material_revert", "material_reject"):
        row = previews.get(stage)
        if not row:
            missing.append(f"{stage}: no actual preview API result")
        elif (row["status"] != ("Failed" if stage == "material_reject" else "Completed") or
              row["liveSwapCommitted"] != (stage != "material_reject") or row["replacesLiveResource"]):
            failures.append(f"{stage}: unexpected preview API result: {row}")
    feedback = [json.loads(line) for line in re.findall(r"EDITOR_S4_FEEDBACK (\{[^\r\n]+\})", text)]
    if any(row["stage"] == "baseline" and "Terrain source changed:" in row["message"] for row in feedback):
        failures.append("Fresh bootstrap Cook was reported stale at the untouched baseline")
    external = [json.loads(line) for line in re.findall(r"EDITOR_S4_EXTERNAL (\{[^\r\n]+\})", text)]
    injected = next((row for row in external if row["event"] == "injected"), None)
    restored = next((row for row in external if row["event"] == "restored"), None)
    conflict_rows = [row for row in feedback if row["stage"] == "conflict"]
    observation_ms = max((row.get("stageElapsedMs", 0) for row in conflict_rows), default=0)
    fresh_conflict = any(
        row.get("feedbackChanged", True) and "unsaved edits conflict" in row["message"]
        and placement in row["message"] and injected is not None
        and row.get("hostTickSerial", 0) > injected["hostTickSerial"]
        and row.get("elapsedMs", 0) > injected["elapsedMs"] for row in conflict_rows)
    if not fresh_conflict:
        missing.append("No actual EditorHost unsaved/external conflict feedback observed")
    if not injected or not restored:
        missing.append("External injection/restore timestamps were not recorded")
    if observation_ms < 1500:
        missing.append(f"External conflict observation window {observation_ms}ms is below 1500ms")
    generations = {state["generation"] for state in states.values()}
    if len(generations) != 1:
        failures.append("World generation changed during the edit sequence")

    for previous, stage in zip(STAGES, STAGES[1:]):
        if previous not in histories or stage not in histories:
            continue
        before = int(histories[previous][-1]["revision"])
        after = int(histories[stage][-1]["revision"])
        advances = stage in ("paint", "paint_undo", "paint_redo", "sculpt", "sculpt_undo",
                             "sculpt_redo", "rapid", "material", "material_revert")
        if (after <= before if advances else after != before):
            failures.append(f"{stage}: unexpected revision {before} -> {after}")
        if stage in ("paint", "sculpt") and stage in states:
            peak = max(int(row["dirty"]) for row in histories[stage])
            if peak == 0:
                missing.append(f"{stage}: no dirty window sampled")
            elif peak >= states[stage]["totalPages"]:
                failures.append(f"{stage}: small brush dirtied every virtual page")

    stale = obsolete.get("rapid", [])
    previous_stale = obsolete.get("sculpt_redo", [0])[-1:]
    discarded = [int(row.get("discarded", 0)) for row in histories.get("rapid", [])]
    previous_discarded = int(histories.get("sculpt_redo", [{}])[-1].get("discarded", 0))
    if not (stale and previous_stale and max(stale) > previous_stale[-1] or
            discarded and max(discarded) > previous_discarded):
        missing.append("No rejected obsolete task observed; rapid edits alone do not prove stale rejection")
    stamps = [json.loads(line) for line in re.findall(r"EDITOR_S4_STAMP (\{[^\r\n]+\})", text)]
    if len(stamps) != 25 or [row["completedStamp"] for row in stamps] != list(range(-1, 24)):
        missing.append("Rapid requires baseline plus 24 ordered per-stamp WorldHash snapshots")
    elif any(before["worldHash"] == after["worldHash"] for before, after in zip(stamps, stamps[1:])):
        failures.append("A rapid Paint/Erase stamp did not actually change WorldHash")

    captures = {}
    for stage in STAGES:
        path = directory / f"{stage}.bmp"
        if not path.exists():
            missing.append(f"{stage}: screenshot missing")
            continue
        try:
            captures[stage] = bmp_integrity(path, None)
        except ValueError as error:
            failures.append(f"{stage}: {error}")
    comparisons = {}
    restoration_masks = {
        ("baseline", "paint_undo"): ("baseline", "paint"),
        ("paint", "paint_redo"): ("baseline", "paint"),
        ("paint_redo", "sculpt_undo"): ("paint_redo", "sculpt"),
        ("sculpt", "sculpt_redo"): ("paint_redo", "sculpt"),
        ("rapid", "material_revert"): ("rapid", "material"),
    }
    changed_pairs = (("baseline", "paint"), ("paint_redo", "sculpt"), ("rapid", "material"))
    for first, second in (*changed_pairs,
                          ("baseline", "paint_undo"), ("paint", "paint_redo"),
                          ("paint_redo", "sculpt_undo"), ("sculpt", "sculpt_redo"),
                          ("rapid", "material_revert"), ("rapid", "conflict"),
                          ("rapid", "conflict_recover"), ("rapid", "save")):
        if first in captures and second in captures:
            first_pixels = rgb_pixels(directory / f"{first}.bmp")
            second_pixels = rgb_pixels(directory / f"{second}.bmp")
            if len(first_pixels) != len(second_pixels):
                failures.append(f"{first} -> {second}: screenshot dimensions changed")
                continue
            difference = sum(abs(left - right) for left, right in zip(first_pixels, second_pixels))
            comparisons[f"{first}:{second}"] = {"meanAbsoluteRgbDifference": difference / len(first_pixels)}
            if (first, second) in changed_pairs and difference == 0:
                failures.append(f"{first} -> {second}: GPU screenshot did not change")
            if (first, second) not in changed_pairs and difference / len(first_pixels) > 1:
                failures.append(f"{first} -> {second}: Undo/Redo output differs by more than 1 RGB level on average")
            reference = restoration_masks.get((first, second))
            if reference and all(name in captures for name in reference):
                before = rgb_pixels(directory / f"{reference[0]}.bmp")
                after = rgb_pixels(directory / f"{reference[1]}.bmp")
                if len(before) == len(after) == len(first_pixels):
                    selected = [offset for offset in range(0, len(before), 3)
                                if max(abs(before[offset + channel] - after[offset + channel])
                                       for channel in range(3)) >= 2]
                    if not selected:
                        missing.append(f"{first} -> {second}: no edited-pixel mask at two RGB levels")
                    else:
                        residual = sum(abs(first_pixels[offset + channel] - second_pixels[offset + channel])
                                       for offset in selected for channel in range(3)) / (3 * len(selected))
                        comparisons[f"{first}:{second}"].update(
                            editedPixelCount=len(selected), editedPixelResidual=residual)
                        if residual > 1:
                            failures.append(f"{first} -> {second}: edited-pixel restoration residual {residual:.6f} > 1")
    if "saved: " not in text or "Heightmap saved: " not in text:
        missing.append("Missing SavePaint/SaveHeightmap success feedback; check save-files.json")
    errors = re.findall(
        r"VUID-[^\s]+|Validation Error[^\n]*|VK_ERROR_[A-Z_]+|"
        r"Terrain (?:paint|sculpt) [^\n]*failed[^\n]*|"
        r"Terrain composited service:[^\n]*|terminate called[^\n]*", text, re.I)
    failures.extend(errors)
    process_path = directory / "process.json"
    if not process_path.exists():
        missing.append("No process exit evidence")
    elif read_json(process_path)["exitCode"] != 0:
        failures.append("Renderer process did not exit successfully")
    saved_path = directory / "save-files.json"
    if "save" not in states:
        missing.append("Save did not complete; persisted files are not a Save failure verdict")
    elif not saved_path.exists():
        missing.append("No persisted save file evidence")
    else:
        saved = read_json(saved_path)
        if set(saved) != {"heightmapPath", "paintPath"}:
            failures.append("Save evidence must include exactly heightmapPath and paintPath")
        for name, evidence in saved.items():
            if not evidence["after"]:
                failures.append(f"Save did not produce {name}")
            elif evidence["before"] == evidence["after"]:
                failures.append(f"Save did not change {name}")
        manifest = saved.get("paintPath", {}).get("manifest", {})
        if (manifest.get("kind") != "terrainLayerWeights" or manifest.get("version") != 2 or
                manifest.get("contentSha256") != saved.get("paintPath", {}).get("after")):
            failures.append("Save Paint manifest is absent or does not authenticate saved bytes")
        # Feedback is stored in the editor view; console logging is not guaranteed.
        if all(value["after"] and value["after"] != value["before"]
               for value in saved.values()) and len(saved) == 2:
            missing = [item for item in missing if not item.startswith("Missing SavePaint/SaveHeightmap")]
    lifecycle = verify_lifecycle(directory, states.get("save"), failures, missing)
    reached = re.findall(r"EDITOR_S4_STAGE ([a-z_]+)", text)
    report = {"failures": sorted(set(failures)), "missingEvidence": sorted(set(missing)),
              "execution": {"reachedStages": reached, "lastReachedStage": reached[-1] if reached else None,
                            "unreachedStages": [name for name in STAGES if name not in reached],
                            "completedSnapshots": list(states)},
              "states": states, "captures": captures, "histories": histories,
              "gpuComparisons": comparisons,
              "dirtyNonresident": {name: int(rows[-1]["dirty"]) for name, rows in histories.items()
                                   if int(rows[-1]["stale"]) == 0 and int(rows[-1]["queued"]) == 0
                                   and int(rows[-1]["reading"]) == 0
                                   and working_set.get(name, {}).get("previewReadbacks") == 0
                                   and working_set.get(name, {}).get("pendingEditorUpdates") == 0
                                   and working_set.get(name, {}).get("pendingMaterialUpdates") == 0},
              "residentWorkingSet": working_set,
              "externalConflictObservation": {"events": external, "feedbackSamples": conflict_rows,
                                               "observationMs": observation_ms,
                                               "freshConflictObserved": fresh_conflict},
              "rapidWorldSnapshots": stamps,
              "obsoleteTaskEvidence": {
                  "staleBakeResultsBefore": previous_stale[-1] if previous_stale else None,
                  "staleBakeResultsRapidMax": max(stale) if stale else None,
                  "discardedBefore": previous_discarded,
                  "discardedRapidMax": max(discarded) if discarded else None},
              "lifecycle": lifecycle,
              "scope": "Actual EngineLoop CommandBus -> EditorHost actions. CPU hashes are World snapshots. "
                       "Screenshot change proves output change only, not channel correctness. "
                       "Conflict comes from actual EditorHost feedback; private recook guard is not exposed. "
                       "Saved snapshot is Cooked in a separate process, reopened and World-reloaded. "
                       "Deterministic delayed GPU completion injection remains a separate gate."}
    write_json(directory / report_name, report)
    return 2 if failures else 3 if missing else 0


def verify_lifecycle(directory, saved, failures, missing):
    path = directory / "lifecycle.json"
    if not path.is_file():
        missing.append("No independent saved-snapshot Cook/reopen/reload evidence")
        return {}
    lifecycle = read_json(path)
    for name in ("bootstrapCook", "savedCook", "reopen"):
        process = lifecycle.get(name, {})
        if process.get("exitCode") != 0 or process.get("timeout") or any(process.get("detectedErrors", {}).values()):
            failures.append(f"{name}: subprocess failed or emitted renderer errors")
    if lifecycle.get("authorBeforeCook") != lifecycle.get("authorAfterCook"):
        failures.append("Cook mutated the saved author snapshot")
    if not lifecycle.get("cookedAfterSave") or lifecycle.get("cookedAfterSave") == lifecycle.get("cookedBeforeSave"):
        failures.append("Cook did not publish a new saved-snapshot dependency identity")
    reopened = lifecycle.get("snapshots", {})
    if not saved or set(reopened) != {"reopen", "reload"}:
        missing.append("Missing save/reopen/reload World snapshots")
        return lifecycle
    for name, snapshot in reopened.items():
        for field in ("height16", "paint", "liveUv", "materialDiskHash", "placement"):
            if snapshot[field] != saved[field]:
                failures.append(f"{name}: saved snapshot {field} did not survive Cook/reload")
        try:
            first = rgb_pixels(directory / "save.bmp")
            second = rgb_pixels(directory / f"{name}.bmp")
            if len(first) != len(second) or sum(abs(a - b) for a, b in zip(first, second)) / len(first) > 1:
                failures.append(f"{name}: GPU output does not match saved preview")
        except (OSError, ValueError) as error:
            missing.append(f"{name}: GPU comparison unavailable: {error}")
    if reopened["reload"]["generation"] <= reopened["reopen"]["generation"]:
        failures.append("Reload did not create a new World generation")
    if not lifecycle.get("reopenSettled"):
        failures.append("Reopened/reloaded terrain queues did not settle")
    return lifecycle


def rgb_pixels(path):
    bmp_integrity(path, None)
    raw = path.read_bytes()
    offset = struct.unpack_from("<I", raw, 10)[0]
    width, height = struct.unpack_from("<ii", raw, 18)
    bits = struct.unpack_from("<H", raw, 28)[0]
    stride = ((width * bits + 31) // 32) * 4
    result = bytearray()
    rows = range(abs(height)) if height < 0 else reversed(range(height))
    for row in rows:
        for column in range(width):
            start = offset + row * stride + column * (bits // 8)
            result.extend(raw[start:start + 3])
    return result


def child(command, staged, environment, log_path, timeout, mode, output):
    overrides = {key: environment[key] for key in
                 ("VL_EDITOR_S4_PLACEMENT", "VL_EDITOR_S4_FRAMES", "VL_EDITOR_S4_STAGE_MS", "PATH")}
    overrides.update(VL_EDITOR_S4_MODE=mode, VL_EDITOR_S4_OUTPUT=str(output))
    previous = {key: os.environ.get(key) for key in overrides}
    os.environ.update(overrides)
    try:
        return run_process(command, staged, log_path, timeout)
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def cooked_identities(resource, scene):
    result = {}
    for obj in read_json(inside(resource, scene))["objects"]:
        if obj.get("type") != "terrain":
            continue
        terrain = read_json(inside(resource, obj["terrainPath"]))
        manifest = inside(resource, terrain["compositedVT"]["packagePath"])
        if manifest.exists():
            result[obj["name"]] = [
                {name: surface[name] for name in ("contentHash", "dependencyHash")}
                for surface in read_json(manifest)["surfaces"]]
    return result


def author_snapshot(resource, scene):
    paths = {inside(resource, scene)}
    for obj in read_json(inside(resource, scene))["objects"]:
        if obj.get("type") != "terrain":
            continue
        terrain = read_json(inside(resource, obj["terrainPath"]))
        paths.add(inside(resource, obj["terrainPath"]))
        for key in ("heightmapPath", "paintPath", "materialInstancePath", "detailMaterialInstancePath"):
            if terrain.get(key):
                paths.add(inside(resource, terrain[key]))
        if terrain.get("paintPath"):
            paths.add(inside(resource, terrain["paintPath"] + ".meta.json"))
        manifest_path = inside(resource, terrain["compositedVT"]["packagePath"])
        if manifest_path.is_file():
            for surface in read_json(manifest_path)["surfaces"]:
                dependencies = read_json(manifest_path.parent / (surface["dependencyHash"] + ".dependencies.json"))
                for entry in dependencies["dependencies"]:
                    if entry["root"] == "resource" and entry["mode"] != "absent":
                        paths.add(inside(resource, entry["path"]))
    return {path.relative_to(resource).as_posix(): digest(path) for path in sorted(paths) if path.is_file()}


def run(options):
    directory = options.output.resolve()
    resource = options.resource_root.resolve(strict=True)
    if directory.is_relative_to(resource) or resource.is_relative_to(directory):
        raise ValueError("Output and original resource root must be disjoint")
    scene = inside(resource, options.scene)
    document = read_json(scene)
    objects = [item for item in document["objects"] if item.get("type") == "terrain"
               and (options.placement is None or "terrain|" + item["name"] == options.placement)]
    if len(objects) != 1:
        raise ValueError("Placement must identify exactly one scene terrain")
    options.placement = "terrain|" + objects[0]["name"]
    terrain = read_json(inside(resource, objects[0]["terrainPath"]))
    if not terrain.get("paintPath") or len(terrain.get("layers", [])) < 2:
        raise ValueError("Fixture needs paintPath and at least two layers")
    directory.mkdir(parents=True, exist_ok=False)
    stage_root = options.staging_root.resolve() if options.staging_root else Path(tempfile.gettempdir()) / "vls4"
    stage_root.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="e4-", dir=stage_root))
    copied = temporary / "r"
    closure_report = prepare(resource, options.scene, copied)
    write_json(directory / "staging.json", {
        "path": str(temporary), "retained": True, "closure": closure_report})
    staged = temporary / "p"
    staged.mkdir()
    stage_project(PROJECT, staged, copied)
    executable = staged / "bin/editor_s4.exe"
    executable.parent.mkdir()
    shutil.copy2(options.exe.resolve(strict=True), executable)
    save_paths = {name: inside(copied, terrain[name]) for name in ("heightmapPath", "paintPath")}
    before = {name: digest(path) if path.exists() else None for name, path in save_paths.items()}
    environment = os.environ.copy()
    environment.update(VL_EDITOR_S4_OUTPUT=str(directory),
                       VL_EDITOR_S4_PLACEMENT=options.placement,
                       VL_EDITOR_S4_FRAMES=str(options.frames),
                       VL_EDITOR_S4_STAGE_MS=str(options.minimum_stage_ms))
    environment["PATH"] = str(options.build.resolve() / "bin") + os.pathsep + environment["PATH"]
    common = [str(executable), "--initial-scene", options.scene, "--no-dev-ui",
               "--validation", "--worker-thread-count", str(options.worker),
               "--frame-limit", str(options.frame_limit)]
    stage_frame_budget = options.frames + (options.minimum_stage_ms * options.frame_limit + 999) // 1000
    command = common + ["--exit-after-frames", str(stage_frame_budget * len(STAGES) + 60)]
    write_json(directory / "invocation.json", {
        "argv": command, "environment": {key: value for key, value in environment.items()
                                        if key.startswith("VL_EDITOR_S4_")},
        "exeSha256": digest(executable), "originalResources": str(resource),
        "copiedResources": str(copied), "placement": options.placement,
    })
    cook_command = common + ["--terrain-vt-cook", "--exit-after-frames", "2"]
    bootstrap, _ = child(cook_command, staged, environment, directory / "bootstrap-cook.log",
                         options.timeout, "cook", directory / "bootstrap")
    if bootstrap["exitCode"] != 0 or bootstrap["timeout"] or any(bootstrap["detectedErrors"].values()):
        write_json(directory / "bootstrap-failure.json", bootstrap)
        return 2
    cooked_before = cooked_identities(copied, options.scene)
    process, _ = child(command, staged, environment, directory / "editor.log",
                       options.timeout, "edit", directory)
    write_json(directory / "process.json", process)
    saved = {
        name: {"before": before[name], "after": digest(path) if path.exists() else None}
        for name, path in save_paths.items()}
    manifest_path = Path(str(save_paths["paintPath"]) + ".meta.json")
    saved["paintPath"]["manifest"] = read_json(manifest_path) if manifest_path.exists() else {}
    write_json(directory / "save-files.json", saved)
    if process["exitCode"] != 0 or process["timeout"] or any(process["detectedErrors"].values()):
        return analyze(directory, options.placement)
    author_before = author_snapshot(copied, options.scene)
    saved_cook, _ = child(cook_command, staged, environment, directory / "saved-cook.log",
                          options.timeout, "cook", directory / "saved-cook")
    author_after = author_snapshot(copied, options.scene)
    lifecycle = {"bootstrapCook": bootstrap, "savedCook": saved_cook,
                 "cookedBeforeSave": cooked_before, "cookedAfterSave": cooked_identities(copied, options.scene),
                 "authorBeforeCook": author_before, "authorAfterCook": author_after}
    if saved_cook["exitCode"] == 0 and not saved_cook["timeout"]:
        reopen, text = child(common + ["--exit-after-frames", str(stage_frame_budget * 2 + 60)],
                             staged, environment, directory / "reopen.log", options.timeout,
                             "reopen", directory)
        lifecycle["reopen"] = reopen
        lifecycle["snapshots"] = {
            row["stage"]: row for row in (
                json.loads(value) for value in re.findall(r"EDITOR_S4_STATE (\{[^\r\n]+\})", text))}
        count = len(cooked_before)
        settled = []
        for section in text.split("EDITOR_S4_STAGE ")[1:]:
            _, problems = editor_stats(section, count)
            settled.append(not problems)
        lifecycle["reopenSettled"] = len(settled) == 2 and all(settled)
    write_json(directory / "lifecycle.json", lifecycle)
    return analyze(directory, options.placement)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build", help="Compile/link only; never starts the renderer")
    builder.add_argument("--build-dir", "--build", dest="build", type=Path, default=PROJECT / "build-vt-release")
    builder.add_argument("--output", type=Path, required=True)
    analyzer = commands.add_parser("analyze")
    analyzer.add_argument("--output", type=Path, required=True)
    analyzer.add_argument("--placement", required=True)
    analyzer.add_argument("--report-name", default="editor-s4-report.json",
                          help="Use a new name to preserve the original run verdict")
    runner = commands.add_parser("run", help="Requires the coordinator's exclusive renderer window")
    runner.add_argument("--exe", type=Path, required=True)
    runner.add_argument("--build-dir", "--build", dest="build", type=Path, default=PROJECT / "build-vt-release")
    runner.add_argument("--output", type=Path, required=True)
    runner.add_argument("--resource-root", type=Path, required=True)
    runner.add_argument("--scene", default=DEFAULT_SCENE)
    runner.add_argument("--staging-root", type=Path, help="Short path, e.g. D:/s4; defaults to TEMP/vls4")
    runner.add_argument("--placement", help="Defaults to the sole terrain identity; explicit mismatches fail")
    runner.add_argument("--frames", type=int, choices=range(30, 3601), default=180, metavar="30..3600")
    runner.add_argument("--frame-limit", type=int, choices=range(1, 1001), default=60, metavar="1..1000")
    runner.add_argument("--minimum-stage-ms", type=int, choices=range(1500, 30001), default=2000,
                        metavar="1500..30000")
    runner.add_argument("--worker", type=int, choices=(1, 2), default=2)
    runner.add_argument("--timeout", type=int, default=600)
    options = parser.parse_args()
    if options.command == "build":
        print(build(options.build, options.output))
        return 0
    if options.command == "analyze":
        return analyze(options.output.resolve(), options.placement, options.report_name)
    return run(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
