"""Prepare/run/analyze V2 Editor evidence; prepare and analyze never start a renderer."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

from build_composited_fixture import validate_author_world, published_world_files, V2_SCENE
from editor_s4_prepare import closure, PROJECT
from editor_s4 import rgb_pixels
from run_composited_acceptance import digest, inside, read_json, write_json, stage_project, run_process, bmp_integrity


def scenario(surface_a, surface_b, link):
    def brush(kind, operation, surface=surface_a, **values):
        return dict(kind=kind, operation=operation, surface=surface, **values)

    def stroke(kind, surface, **values):
        return [brush(kind, operation, surface, **values) for operation in ("begin", "apply", "end")]

    edge = link["sourceEdge"]
    boundary = dict(u=0.995 if edge == "maxU" else 0.005 if edge == "minU" else 0.5,
                    v=0.995 if edge == "maxV" else 0.005 if edge == "minV" else 0.5)
    steps = []

    def step(name, operations=()):
        steps.append({"id": name, "operations": list(operations)})

    step("baseline")
    step("paint_a", stroke("paint", surface_a))
    step("paint_undo", [brush("paint", "undo")])
    step("paint_redo", [brush("paint", "redo")])
    step("paint_pending_cancel", [brush("paint", "begin"), brush("paint", "apply", u=0.6)])
    step("paint_cancel", [brush("paint", "cancel")])
    step("sculpt_a", stroke("sculpt", surface_a))
    step("sculpt_undo", [brush("sculpt", "undo")])
    step("sculpt_redo", [brush("sculpt", "redo")])
    step("sculpt_pending_cancel", [brush("sculpt", "begin"), brush("sculpt", "apply", u=0.6)])
    step("sculpt_cancel", [brush("sculpt", "cancel")])
    step("boundary_paint", stroke("paint", surface_a, **boundary))
    step("boundary_sculpt", stroke("sculpt", surface_a, **boundary))
    step("cross_surface", stroke("paint", surface_b) + stroke("sculpt", surface_b))
    step("rapid", sum((stroke("paint", surface_b, erase=index % 2 == 0)
                       for index in range(12)), []))
    step("material", [{"kind": "material"}])
    step("material_revert", [{"kind": "materialRestore"}])
    step("material_old_world_reject", [{"kind": "materialReject"}])
    step("hotreload", [{"kind": "hotReload"}])
    step("hotreload_restore", [{"kind": "hotReloadRestore"}])
    step("preview_published")
    step("save_a", [brush("paint", "savePaint"), brush("sculpt", "saveHeight")])
    step("save_b", [brush("paint", "savePaint", surface_b), brush("sculpt", "saveHeight", surface_b)])
    step("saved_preview")
    step("reload_inflight", stroke("paint", surface_b) + [{"kind": "reloadWorld"}])
    step("old_world_action", [brush("paint", operation, surface_b, oldGeneration=True)
                              for operation in ("begin", "apply", "end")])
    step("reloaded")
    return {"kind": "editor-s4-v2-plan", "version": 1, "surfaceA": surface_a,
            "surfaceB": surface_b, "adjacency": link, "steps": steps,
            "fullCookPrerequisite": False, "requiresExistingValidatedV2CoarsePublication": True}


# E3: the layer-parameter plan drives a real TR edit through the editor asset API
# (Console `editor <JSON>` facade: get_document -> modify -> validate -> save) and
# then reloads the World, because a TR layer parameter is a production/catalog
# input and must not be live-patched.
LAYER_EDIT_VALUES = {"uvScale": 6.0, "noise": 0.35}


def layer_plan(surface_a, surface_b, link, targets, values, prefix="layer_param_edit",
               ids=("baseline", "layer_param_reload", "layer_param_reloaded")):
    """One TR document edit per step, then a reload step.

    A layer key is World-global and every TR that repeats the key must carry the
    same parameters (TerrainLayerRegistry::LayersEquivalent), so a real layer edit
    touches every TR that authors that layer; editing one tile would be rejected
    by the World load with "Terrain layer key conflict".
    """
    plan = scenario(surface_a, surface_b, link)
    steps = [{"id": ids[0], "operations": []}]
    for index, (terrain_path, layer_name) in enumerate(targets):
        changes = [{"field": field, "layerIndex": 1, "expectedLayerName": layer_name,
                    "value": value} for field, value in values.items()]
        steps.append({"id": f"{prefix}_{index:02d}", "operations": [{
            "kind": "layerParameter", "assetPath": terrain_path,
            "expectedLayerName": layer_name, "changes": changes, "reload": False,
            "step": f"{prefix}_{index:02d}"}]})
    steps.append({"id": ids[1], "operations": [{"kind": "reloadWorld"}]})
    steps.append({"id": ids[2], "operations": []})
    plan["steps"] = steps
    plan["layerEdits"] = {"targets": len(targets), "values": values}
    return plan


def baseline_plan(surface_a, surface_b, link, ids=("baseline",)):
    plan = scenario(surface_a, surface_b, link)
    plan["steps"] = [{"id": name, "operations": []} for name in ids]
    return plan



def prepare(resource, scene, stage, worker, expected_surfaces, plan_kind="matrix",
            shader_spv_root=None):
    resource = resource.resolve(strict=True)
    stage = stage.resolve()
    if stage.exists() or stage.is_relative_to(resource) or resource.is_relative_to(stage):
        raise ValueError("Stage must be new and disjoint from source resources")
    if stage.is_relative_to(PROJECT) or PROJECT.is_relative_to(stage):
        raise ValueError("Use a short private stage outside the project, e.g. D:/s4v2/w2")
    world = validate_author_world(resource, scene, expected_surfaces)
    document = read_json(inside(resource, scene))
    placements = {f"terrain|{item['name']}": item for item in document["objects"]
                  if item.get("type") == "terrain"}
    first = read_json(inside(resource, next(iter(placements.values()))["terrainPath"]))
    settings = first["compositedVT"]
    recipe = read_json(inside(resource, settings["productionSourcePath"]))
    if not recipe["adjacency"]:
        raise ValueError("Cross-surface evidence requires explicit adjacent placements")
    link = recipe["adjacency"][0]
    plan = scenario(link["sourceSurfaceId"], link["targetSurfaceId"], link)
    surface_tr = read_json(inside(resource, placements[link["sourceSurfaceId"]]["terrainPath"]))
    layer_targets = [(item["terrainPath"], read_json(inside(resource, item["terrainPath"]))["layers"][1]["name"])
                     for item in placements.values()]
    if plan_kind == "layer":
        plan = layer_plan(link["sourceSurfaceId"], link["targetSurfaceId"], link,
                          layer_targets, LAYER_EDIT_VALUES)
    elif plan_kind == "layer-restore":
        restore_path = os.environ.get("VL_EDITOR_S4_RESTORE_VALUES")
        if not restore_path:
            raise ValueError("layer-restore needs VL_EDITOR_S4_RESTORE_VALUES (JSON of field:value)")
        restore = json.loads(Path(restore_path).read_text(encoding="utf-8"))
        plan = layer_plan(link["sourceSurfaceId"], link["targetSurfaceId"], link,
                          layer_targets, restore, prefix="layer_param_restore",
                          ids=("restore_before", "restore_reload", "restore_after"))
    elif plan_kind == "baseline":
        plan = baseline_plan(link["sourceSurfaceId"], link["targetSurfaceId"], link)
    elif plan_kind != "matrix":
        raise ValueError("Unknown plan kind: " + plan_kind)
    manifest = inside(resource, settings["packagePath"])
    publication = read_json(manifest)
    if publication.get("version") != 2:
        raise ValueError("V2 publication required; no bootstrap full Cook or V1 fallback")
    for selected in (plan["surfaceA"], plan["surfaceB"]):
        published = next((item for item in publication["production"]["surfaces"]
                          if item["surfaceId"] == selected), None)
        if not published or not published.get("authorDependencies"):
            raise ValueError(f"Publication lacks Cook-captured authorDependencies for {selected}; "
                             "do not reconstruct a baseline from current author files")
    package_files = published_world_files(resource, settings["packagePath"], placements)
    files, optional = closure(resource, scene)
    files = set(files) | set(package_files.values())
    for identity in (plan["surfaceA"], plan["surfaceB"]):
        terrain = read_json(inside(resource, placements[identity]["terrainPath"]))
        if not terrain.get("paintPath") or len(terrain.get("layers", [])) < 2:
            raise ValueError("Both selected terrains need paintPath and at least two layers")
        if terrain["paintPath"] == terrain["heightmapPath"]:
            raise ValueError("Author save paths must be distinct")
    for path in files:
        if path.name.endswith(".dependencies.json"):
            raise ValueError("V1 dependency sidecar reached V2 closure")
        if len(str(stage / "r" / path.relative_to(resource))) >= 240:
            raise ValueError("Private fixture exceeds Windows path limit")
    copied = stage / "r"
    copied.mkdir(parents=True)
    (copied / "Maps").mkdir(exist_ok=True)
    for source in sorted(files):
        target = copied / source.relative_to(resource)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    document["runtimeMode"] = "authoring"
    write_json(inside(copied, scene), document)
    project = stage / "p"
    project.mkdir()
    stage_project(PROJECT, project, copied)
    # 固定 SPV 闭包：stage 的 shader/spv 必须来自本次发布的游戏缓存归档，
    # 而不是工作树里最近一次本地编译留下的输出。归档是私有只读输入，
    # 这里只替换 stage 自己的副本，从不回写工作树。
    spv_source = None
    if shader_spv_root is not None:
        spv_source = shader_spv_root.resolve(strict=True)
        if not (spv_source / "shader-build-cache.json").is_file():
            raise ValueError("SPV closure root lacks shader-build-cache.json")
        shutil.rmtree(project / "shader/spv")
        shutil.copytree(spv_source, project / "shader/spv", copy_function=shutil.copy2)
    write_json(project / "editor-s4-v2-marker.json", {"privateStage": str(stage)})
    plan.update(scene=scene, worker=worker, framesPerStep=180)
    write_json(stage / "plan.json", plan)
    # 阶段元数据：不改 plan.json（27 步矩阵文件与已发布证据保持同形），
    # 只把 E3/E7 需要的作者身份单列一份。
    write_json(stage / "meta.json", {
        "scene": scene,
        "worker": worker,
        "surfaceATerrainPath": placements[link["sourceSurfaceId"]]["terrainPath"],
        "surfaceALayerName": surface_tr["layers"][1]["name"],
        "layerTargets": [list(target) for target in layer_targets],
        "packagePath": settings["packagePath"],
        "resourceRoot": str(copied),
        "projectRoot": str(project),
        "planKind": plan_kind})
    inventory = {path.relative_to(resource).as_posix():
                 {"bytes": path.stat().st_size, "sha256": digest(path)} for path in sorted(files)}
    write_json(stage / "preparation.json", {
        "kind": "editor-s4-v2-private-stage", "scene": scene, "source": str(resource),
        "files": inventory, "optionalMissingOutputs": optional,
        "publicationManifest": settings["packagePath"],
        "publicationBefore": digest(copied / settings["packagePath"]),
        "shaderSpvRoot": str(spv_source) if spv_source else None,
        "shaderSpvClosure": ({"files": sum(1 for _ in spv_source.rglob("*") if _.is_file()),
                              "cacheSha256": digest(spv_source / "shader-build-cache.json")}
                             if spv_source else None),
        "planKind": plan_kind,
        "rendererStarted": False, "cookStarted": False, "copyMethod": "copy2; no hardlinks",
        "worldSurfaceCount": len(world["terrains"]) if "terrains" in world else len(placements)})
    return plan


def run(stage, exe, runtime_bin, timeout):
    stage = stage.resolve(strict=True)
    plan = read_json(stage / "plan.json")
    project = stage / "p"
    target = project / "bin/editor_s4.exe"
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(exe.resolve(strict=True), target)
    output = stage / "evidence"
    output.mkdir(exist_ok=False)
    # 180 ticks/step and step order are unchanged. The extra 600-frame budget is
    # only the bounded loading-gate wait the adapter may spend holding a step's
    # last phase before it requests that step's screenshot; it is not a step
    # length or an acceptance threshold.
    command = [str(target), "--initial-scene", plan["scene"], "--no-dev-ui", "--validation",
               "--worker-thread-count", str(plan["worker"]), "--frame-limit", "60",
               "--exit-after-frames", str(len(plan["steps"]) * plan["framesPerStep"] + 120 + 600)]
    overrides = {
        "VL_EDITOR_S4_V2_PLAN": str(stage / "plan.json"),
        "VL_EDITOR_S4_OUTPUT": str(output), "VL_EDITOR_S4_PLACEMENT": plan["surfaceA"],
        "VL_EDITOR_S4_MODE": "edit", "VL_EDITOR_S4_FRAMES": str(plan["framesPerStep"]),
        "PATH": str(runtime_bin.resolve(strict=True)) + os.pathsep + os.environ["PATH"]}
    previous = {key: os.environ.get(key) for key in overrides}
    try:
        os.environ.update(overrides)
        write_json(output / "invocation.json", {
            "argv": command, "environment": {key: value for key, value in overrides.items() if key != "PATH"},
            "exeSha256": digest(target), "fullCookStarted": False})
        process, _ = run_process(command, project, output / "editor.log", timeout)
        write_json(output / "process.json", process)
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    return analyze(stage)


def native_checks(native):
    failures, missing = [], []
    required = {"queue", "prepared", "submitted", "published", "stale-rejected",
                "page-table-write", "baseline-v2", "dependency-reload", "frame-input-immutable",
                "neighbor-dirty", "author-save", "dirty-ack"}
    missing.extend(f"native telemetry {name}" for name in sorted(required - {row["event"] for row in native}))
    histories = {}
    for index, row in enumerate(native):
        kind = row["event"]
        if kind in ("queue", "prepared", "submitted", "published"):
            key = (row.get("worldGeneration"), row.get("surface"), row.get("ticket"), row.get("editSeq"))
            histories.setdefault(key, []).append((index, kind))
            if any(value is None for value in key):
                failures.append("Transaction telemetry lacks exact World/surface/ticket/editSeq")
        if kind in ("published", "page-table-write", "dirty-ack") and not (
                row.get("worldGeneration") == row.get("activeWorldGeneration") and
                row.get("revision") == row.get("activeRevision") and
                row.get("contentIdentity") == row.get("activeContentIdentity") and
                all(row.get(field) is not None for field in
                    ("worldGeneration", "revision", "contentIdentity", "ticket", "editSeq", "surface"))):
            failures.append("Native publication token mismatch")
        if kind == "dirty-ack":
            if row.get("state") != "Published":
                failures.append("Dirty acknowledged before Published")
            matching = [item for item in native[:index] if item["event"] == "published" and
                        all(item.get(field) == row.get(field) for field in
                            ("ticket", "worldGeneration", "surface", "editSeq", "revision", "contentIdentity"))]
            if not matching:
                failures.append("Dirty acknowledgement has no prior matching publication")
        if kind == "baseline-v2" and (
                row.get("storeVersion") != 2 or row.get("v1SidecarReads") != 0 or
                row.get("verifiedProductionObjects") is not True or
                row.get("capturedDependencyCount", 0) < 1 or
                row.get("matchesStoredAuthorDependencies") is not True):
            failures.append("Baseline did not exclusively use verified V2 objects")
        if kind == "frame-input-immutable":
            for field in ("weights", "normalFrame", "tangentFrame"):
                values = row.get(field, {})
                if not values.get("beforeDigest") or values.get("beforeDigest") != values.get("afterDigest"):
                    failures.append(f"Producer immutable {field} snapshot changed or lacks hashes")
            if not row.get("retainedThroughFence"):
                missing.append("producer retained immutable weights/frame inputs through fence")
        if kind == "stale-rejected" and (
                row.get("pageTableWritten") is not False or row.get("reason") not in
                ("world-generation", "surface-revision", "content-identity")):
            failures.append("Old task rejection does not prove no page table write")
        if kind == "dependency-reload" and not (
                row.get("beforeDigest") and row.get("afterDigest") and
                row["beforeDigest"] != row["afterDigest"] and
                row.get("candidatePublished") is True):
            missing.append("hot reload must change a dependency and publish its candidate")
        if kind == "neighbor-dirty" and (
                not row.get("sourceSurface") or not row.get("targetSurface") or
                row["sourceSurface"] == row["targetSurface"] or not row.get("pageKeys")):
            missing.append("cross-surface affected page keys")
    for key, rows in histories.items():
        kinds = [kind for _, kind in rows]
        if "published" in kinds:
            if not all(kind in kinds for kind in ("queue", "prepared", "submitted")):
                failures.append("Published ticket lacks Queue/Prepared/Submitted history")
            elif [kinds.index(kind) for kind in ("queue", "prepared", "submitted", "published")] != sorted(
                    kinds.index(kind) for kind in ("queue", "prepared", "submitted", "published")):
                failures.append("Published ticket lifecycle is out of order")
    return failures, missing


def analyze(stage, game_evidence=None):
    plan = read_json(stage / "plan.json")
    output = stage / "evidence"
    text = (output / "editor.log").read_text(encoding="utf-8", errors="replace")
    events = [json.loads(line.split("EDITOR_S4_V2 ", 1)[1])
              for line in text.splitlines() if line.startswith("EDITOR_S4_V2 ")]
    snapshots = {(row["step"], row["surface"]): row for row in events if row["event"] == "snapshot"}
    failures, missing = [], []
    process = read_json(output / "process.json")
    if process["exitCode"] or process["timeout"] or any(process["detectedErrors"].values()):
        failures.append("Editor process/validation failed")
    if "EngineLoop teardown complete" not in text:
        missing.append("clean Editor teardown")
    for step in plan["steps"]:
        for surface in (plan["surfaceA"], plan["surfaceB"]):
            row = snapshots.get((step["id"], surface))
            if not row:
                missing.append(f"snapshot {step['id']} {surface}")
            elif row.get("immutableWorldInputsIntact") is not True:
                failures.append(f"input snapshot mutated {step['id']} {surface}")
        image = output / (step["id"] + ".bmp")
        try:
            info = bmp_integrity(image, None)
            if info["sampledUniqueRgb"] < 2:
                failures.append(f"blank screenshot {step['id']}")
        except (OSError, ValueError, RuntimeError):
            missing.append(f"complete screenshot {step['id']}")
    surface = plan["surfaceA"]
    pairs = [("paint_undo", "baseline", "paintDigest"), ("paint_redo", "paint_a", "paintDigest"),
             ("paint_cancel", "paint_redo", "paintDigest"),
             ("sculpt_undo", "paint_cancel", "heightDigest"),
             ("sculpt_redo", "sculpt_a", "heightDigest"),
             ("sculpt_cancel", "sculpt_redo", "heightDigest")]
    for left, right, field in pairs:
        if (left, surface) in snapshots and (right, surface) in snapshots:
            if snapshots[left, surface][field] != snapshots[right, surface][field]:
                failures.append(f"{left} did not restore {right} {field}")
    pixel_comparisons = {}
    for left, right in (("paint_undo", "baseline"), ("paint_redo", "paint_a"),
                        ("paint_cancel", "paint_redo"), ("sculpt_undo", "paint_cancel"),
                        ("sculpt_redo", "sculpt_a"), ("sculpt_cancel", "sculpt_redo")):
        first, second = output / (left + ".bmp"), output / (right + ".bmp")
        if first.is_file() and second.is_file():
            try:
                before, after = rgb_pixels(first), rgb_pixels(second)
                if len(before) != len(after):
                    failures.append(f"pixel extents differ {left}/{right}")
                    continue
                error = sum(abs(a - b) for a, b in zip(before, after)) / len(before)
                pixel_comparisons[f"{left}/{right}"] = error
                if error > 1:
                    failures.append(f"Undo/Redo/Cancel pixels exceed 1/255 {left}/{right}")
            except (OSError, ValueError):
                missing.append(f"pixel comparison {left}/{right}")
    for changed, before, selected, field in (
            ("paint_a", "baseline", surface, "paintDigest"),
            ("sculpt_a", "paint_cancel", surface, "heightDigest"),
            ("cross_surface", "boundary_sculpt", plan["surfaceB"], "paintDigest"),
            ("cross_surface", "boundary_sculpt", plan["surfaceB"], "heightDigest")):
        if (changed, selected) in snapshots and (before, selected) in snapshots and (
                snapshots[changed, selected][field] == snapshots[before, selected][field]):
            failures.append(f"No actual edit {changed}/{selected}/{field}")
    for selected in (plan["surfaceA"], plan["surfaceB"]):
        base = snapshots.get(("baseline", selected))
        saved = snapshots.get(("saved_preview", selected))
        if base and saved:
            if base["authorHeightDigest"] == saved["authorHeightDigest"]:
                failures.append(f"author height was not saved {selected}")
            if base["authorPaintDigest"] == saved["authorPaintDigest"]:
                failures.append(f"author paint was not saved {selected}")
            if base["gameManifestDigest"] != saved["gameManifestDigest"]:
                failures.append("Author save/preview unexpectedly changed the Game publication")
            if saved["publishedRevision"] <= base["publishedRevision"]:
                failures.append(f"preview did not publish {selected}")
    baseline = snapshots.get(("baseline", surface))
    reloaded = snapshots.get(("reloaded", surface))
    if baseline and reloaded and reloaded["generation"] <= baseline["generation"]:
        failures.append("World generation did not advance")
    telemetry = output / "native-refresh.jsonl"
    required = {"queue", "prepared", "submitted", "published", "stale-rejected",
                "page-table-write", "baseline-v2", "dependency-reload", "frame-input-immutable",
                "neighbor-dirty", "author-save", "dirty-ack"}
    if not telemetry.is_file():
        missing.extend(f"native telemetry {name}" for name in sorted(required))
    else:
        native = [json.loads(line) for line in telemetry.read_text().splitlines() if line.strip()]
        native_failures, native_missing = native_checks(native)
        failures.extend(native_failures)
        missing.extend(native_missing)
        for step in ("paint_a", "paint_undo", "paint_redo", "paint_cancel",
                     "sculpt_a", "sculpt_undo", "sculpt_redo", "sculpt_cancel",
                     "boundary_paint", "boundary_sculpt", "cross_surface", "material",
                     "material_revert"):
            if not any(row["event"] == "published" and row.get("step") == step for row in native):
                missing.append(f"exact Queue/Poll Published receipt for {step}")
        for kind in ("Paint", "Sculpt"):
            if not any(row["event"] == "neighbor-dirty" and row.get("kind") == kind and
                       row.get("sourceSurface") == plan["surfaceA"] and
                       row.get("targetSurface") == plan["surfaceB"] for row in native):
                missing.append(f"neighbor propagation {kind} A to B")
    if not any(row["event"] == "feedback" and row.get("step") == "old_world_action" and
               "World changed" in row.get("message", "") for row in events):
        missing.append("actual old-generation UI action rejection")
    missing.append("independent visual review of both surfaces, seam and normal/TES output")
    if game_evidence is None:
        missing.append("independent Game publication/export/reopen receipt after explicit publish")
    else:
        receipt = read_json(game_evidence)
        if receipt.get("status") != "passed" or receipt.get("storeVersion") != 2 or (
                receipt.get("sourceEditorLogSha256") != digest(output / "editor.log")):
            failures.append("Game publication receipt failed")
        saved_rows = {selected: snapshots.get(("saved_preview", selected))
                      for selected in (plan["surfaceA"], plan["surfaceB"])}
        for selected, saved in saved_rows.items():
            entry = receipt.get("surfaces", {}).get(selected, {})
            if saved and any(entry.get(field) != saved[field] for field in
                             ("authorHeightDigest", "authorPaintDigest")):
                failures.append("Game publication is not bound to saved author inputs")
            if not entry.get("reopenedProductionIdentity") or entry.get("reopenPixelsCompared") is not True:
                missing.append(f"Game reopen pixels and Production identity {selected}")
        if baseline and (receipt.get("beforeManifestDigest") != baseline["gameManifestDigest"] or
                         receipt.get("publishedManifestDigest") == baseline["gameManifestDigest"] or
                         not receipt.get("publishedManifestDigest")):
            failures.append("Explicit Game publication did not advance the manifest identity")
        if not receipt.get("nativeValidatorPassed") or not receipt.get("gameCleanTeardown"):
            missing.append("native Game validator and clean independent Game teardown")
    report = {"status": "failed" if failures else "incomplete" if missing else "passed",
              "failures": failures, "missingEvidence": missing,
              "authorSaved": "checked separately", "previewPublished": "requires exact native ticket trace",
              "gamePublished": "requires independent Game receipt", "independentReview": "pending",
              "gpuEvidenceCollectedByPreparation": False}
    report["pixelComparisonsMeanByteError"] = pixel_comparisons
    write_json(output / "editor-s4-v2-report.json", report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "passed" else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    planner = commands.add_parser("plan", help="Write operations/readiness only; no payload copy, build or GPU")
    planner.add_argument("--resource-root", type=Path, required=True)
    planner.add_argument("--scene", default=V2_SCENE)
    planner.add_argument("--output", type=Path, required=True)
    planner.add_argument("--expected-surfaces", type=int, default=16)
    preparer = commands.add_parser("prepare")
    preparer.add_argument("--resource-root", type=Path, required=True)
    preparer.add_argument("--scene", required=True)
    preparer.add_argument("--stage", type=Path, required=True)
    preparer.add_argument("--worker", type=int, choices=(1, 2), default=2)
    preparer.add_argument("--expected-surfaces", type=int, default=16)
    preparer.add_argument("--plan-kind", choices=("matrix", "layer", "layer-restore", "baseline"),
                          default="matrix")
    preparer.add_argument("--shader-spv-root", type=Path, default=None,
                          help="Published shader/spv closure copied into the private stage")
    runner = commands.add_parser("run", help="Only after Cicero finishes and coordinator releases GPU window")
    runner.add_argument("--stage", type=Path, required=True)
    runner.add_argument("--exe", type=Path, required=True)
    runner.add_argument("--runtime-bin", type=Path, required=True)
    runner.add_argument("--timeout", type=int, default=600)
    checker = commands.add_parser("analyze")
    checker.add_argument("--stage", type=Path, required=True)
    checker.add_argument("--game-evidence", type=Path)
    args = parser.parse_args()
    if args.command == "plan":
        root = args.resource_root.resolve(strict=True)
        world = validate_author_world(root, args.scene, args.expected_surfaces)
        recipe = world["author"]
        if not recipe["adjacency"]:
            raise ValueError("Plan requires explicit adjacent surfaces")
        link = recipe["adjacency"][0]
        plan = scenario(link["sourceSurfaceId"], link["targetSurfaceId"], link)
        plan.update(scene=args.scene, sourceResourceRoot=str(root), rendererStarted=False,
                    cookStarted=False, payloadCopied=False)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if args.output.exists():
            raise ValueError("Preserve existing plan; use a new --output")
        write_json(args.output, plan)
        print(f"Prepared {len(plan['steps'])} steps; no renderer, Cook or payload copy")
        return 0
    if args.command == "prepare":
        prepare(args.resource_root, args.scene, args.stage, args.worker,
                args.expected_surfaces, args.plan_kind, args.shader_spv_root)
        print("Prepared V2 stage; rendererStarted=false; cookStarted=false")
        return 0
    if args.command == "run":
        return run(args.stage, args.exe, args.runtime_bin, args.timeout)
    return analyze(args.stage, args.game_evidence)


if __name__ == "__main__":
    sys.exit(main())
