"""RVT delivery planning and receipt verification; never starts a renderer.

Default mode writes a plan with status planned/incomplete, never passed.
Exit codes: 0 planned or verified, 1 invalid input, 2 failed, 3 incomplete.

--adapter is an explicit Python evidence adapter, not a terrain asset schema.
It must implement normalize_p0(path), inspect_package(package, scene, weight_source)
and verify_case(case, evidence, frozen, package_summary). No default V1/V2 parsing,
weight conversion, threshold values or skipped checks are provided here.

normalize_p0 returns the evidence view validated by validate_frozen below. Its
baseline_receipt and measurement_artifacts paths are relative to the P0 file.
Each performance rule carries baseline_samples, an artifact containing raw
sampling/unit/aggregation/values/sample_ids; minimum sample counts come from P0.
The adapter must preserve the frozen values and verify the actual P0 schema,
freeze state, baseline comparison and measurement provenance.
baseline_valid must be literal True and measurement_run_id must identify the
accepted isolated run. Invalidated/interfered measurements cannot be reused.
inspect_package must validate the real Store/exporter protocol and the entire
installed production closure, including shader identity, roots, height, weight
inputs and adjacency. It returns version=2, surface_ids, weight_source,
content_identity and closure_sha256. It must reject V1 and external dependencies.
verify_case independently checks semantic evidence for every gate in case.gates;
it returns status=passed and checks mapping each gate to literal True.
The observed_run_identity gate must check actual process binary/shader/input,
scene, worker and weight mode provenance, not only copy the receipt declaration.
Missing adapters or evidence are blocking, never successful acceptance.
"""

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys

from check_composited_fixture_camera import png16
from run_composited_acceptance import camera_shots, digest, inside, read_json, write_json


WEIGHT_SOURCES = ("CoverageAtlas", "LocalLayerArray")
ADAPTER_FUNCTIONS = ("normalize_p0", "inspect_package", "verify_case")
BUDGETS = ("cache_workspace_bytes", "shared_inputs_bytes", "terrain_total_bytes",
           "replacement_peak_bytes", "cpu_total_bytes", "pages_per_frame",
           "bytes_per_frame", "inflight_tasks")
METRICS = ("cold_start_ms", "hot_start_ms", "convergence_ms", "cpu_frame_ms",
           "terrain_draw_gpu_ms", "tes_draw_gpu_ms", "production_gpu_ms",
           "copy_gpu_ms", "worst_page_gpu_ms", "process_private_bytes",
           "wddm_dedicated_shared_bytes")
SAMPLING = {
    "cold_start_ms": "per-run", "hot_start_ms": "per-run", "convergence_ms": "per-run",
    "cpu_frame_ms": "per-frame", "terrain_draw_gpu_ms": "gpu-timestamp",
    "tes_draw_gpu_ms": "gpu-timestamp", "production_gpu_ms": "gpu-timestamp",
    "copy_gpu_ms": "gpu-timestamp", "worst_page_gpu_ms": "gpu-timestamp",
    "process_private_bytes": "pid-memory", "wddm_dedicated_shared_bytes": "pid-memory",
}
CASE_GATES = {
    "closure": ("v2_full16", "complete_inputs", "runtime_compute", "offline_paths"),
    "cold": ("empty_fine_cache", "initial_quality", "produced_pages", "converged"),
    "hot": ("same_cache_identity", "cache_scope_recorded", "converged"),
    "tour": ("all_tile_centers", "continuous_crossings", "converged"),
    "teleport": ("unvisited_destinations", "each_arrival_quality", "each_arrival_converged"),
    "seams": ("all_24_edges", "all_9_junctions", "outer_4_corners",
              "coarse_footprints", "mixed_layer_mapping"),
    "tes": ("active_distance", "nonzero_displacement", "height_ready",
            "correct_weight_source", "independent_of_base_residency"),
    "cache_deleted": ("fine_cache_deleted", "produced_pages", "converged"),
    "cache_corrupt": ("corruption_observed", "bounded_rebuild", "converged"),
    "input_corrupt": ("input_integrity_rejected", "no_candidate_commit", "clean_teardown"),
    "root_corrupt": ("root_rebuilt", "no_uninitialized_draw", "installation_verify_rejects"),
    "production_failure": ("nonzero_status", "submission_failure", "no_partial_publish",
                           "bounded_retry", "slot_retirement"),
    "over_budget": ("frozen_caps", "no_peak_overrun", "quality_floor", "no_starvation"),
    "world_replace": ("different_world_roundtrip", "generation_isolation",
                      "overlap_accounted", "failed_candidate_preserves_old"),
    "editor_refresh": ("independent_p4_receipt", "inflight_edits", "no_stale_publish"),
    "v1_reject": ("installation_rejected", "runtime_rejected", "no_fallback", "no_crash"),
    "legacy_switch_reject": ("runtime_rejected", "no_fallback", "no_crash"),
    "offline_game": ("workspace_independent", "read_only_install", "separate_writable_cache",
                     "no_shader_compile", "validation_clean", "clean_teardown"),
}


class IncompleteEvidence(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, name, positive=False):
    require(type(value) in (float, int) and math.isfinite(value)
            and (value > 0 if positive else value >= 0), f"Invalid {name}")
    return value


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode()).hexdigest()


def logical_path(value):
    path = Path(value)
    require(bool(value) and not path.is_absolute() and not path.drive and not path.root
            and ":" not in value and "\\" not in value
            and ".." not in path.parts, "Expected resource-relative forward-slash path")
    return value


def artifact(path):
    path = path.resolve(strict=True)
    require(path.is_file(), f"Not a file: {path}")
    return {"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size}


def checked_artifact(entry, root):
    require(isinstance(entry, dict) and {"path", "sha256", "bytes"} <= entry.keys(),
            "Artifact needs path, sha256 and bytes")
    path = inside(root, logical_path(entry["path"]))
    actual = artifact(path)
    require(actual["sha256"] == entry["sha256"] and actual["bytes"] == entry["bytes"],
            f"Artifact identity mismatch: {path}")
    return path


def load_adapter(path):
    if path is None:
        raise IncompleteEvidence("P0/Store adapter not supplied; acceptance blocked")
    specification = importlib.util.spec_from_file_location("rvt_delivery_adapter", path)
    require(specification is not None and specification.loader is not None, "Invalid adapter")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    for name in ADAPTER_FUNCTIONS:
        require(callable(getattr(module, name, None)), f"Adapter must implement {name}")
    return module


def validate_frozen(frozen, root):
    if isinstance(frozen, dict) and "native_p0" in frozen:
        from rvt_native_adapter import normalize_p0
        entry = frozen["native_p0"]
        source = Path(entry["path"])
        require(artifact(source) == entry, "Native P0 identity changed")
        require(normalize_p0(source) == frozen, "Native P0 mapping differs from frozen evidence")
        return frozen
    require(isinstance(frozen, dict) and frozen.get("status") == "frozen",
            "P0 is not frozen")
    require(frozen.get("baseline_valid") is True, "P0 baseline missing or invalidated")
    require(isinstance(frozen.get("measurement_run_id"), str) and frozen["measurement_run_id"].strip(),
            "Missing accepted P0 measurement run identity")
    for section in ("hardware", "display", "build", "cache_scope", "quality"):
        require(isinstance(frozen.get(section), dict) and frozen[section],
                f"Missing P0 {section}")
    require({"target_mip", "effective_mip", "first_frame", "convergence"}
            <= frozen["quality"].keys(), "Missing frozen quality gates")
    require(all(value is not None and value != "" for value in frozen["quality"].values()),
            "Empty frozen quality gate")
    require(frozen.get("comparison_requirements"), "Missing baseline improvement requirements")
    checked_artifact(frozen.get("baseline_receipt"), root)
    measurements = frozen.get("measurement_artifacts")
    require(isinstance(measurements, list) and measurements, "Missing P0 measurements")
    for entry in measurements:
        path = checked_artifact(entry, root)
        require(path.stat().st_size > 0, f"Empty P0 measurement: {path}")
    budgets = frozen.get("budgets", {})
    for name in BUDGETS:
        number(budgets.get(name), f"P0 budget {name}", positive=True)
    performance = frozen.get("performance", {})
    require(isinstance(performance, dict) and set(METRICS) <= performance.keys(),
            "Missing P0 performance metrics")
    for name, rule in performance.items():
        require(isinstance(rule, dict), f"Missing P0 metric {name}")
        number(rule.get("maximum"), f"{name} maximum", positive=True)
        require(type(rule.get("min_samples")) is int and rule["min_samples"] > 0,
                f"Missing frozen sample count: {name}")
        require(rule.get("statistic") in ("max", "p50", "p95", "p99"),
                f"Unsupported statistic: {name}")
        require(rule.get("sampling") in ("per-frame", "gpu-timestamp", "per-run", "pid-memory"),
                f"Missing raw sampling contract: {name}")
        require(name not in SAMPLING or rule["sampling"] == SAMPLING[name],
                f"Wrong P0 sampling scope: {name}")
        require(rule["statistic"] == "max" or rule["sampling"] in ("per-frame", "gpu-timestamp"),
                "Frame percentiles require raw frames/timestamps, not rolling/1Hz snapshots")
        require(isinstance(rule.get("cases"), list) and rule["cases"]
                and all(case in CASE_GATES for case in rule["cases"]),
                f"Missing metric case scope: {name}")
        require(isinstance(rule.get("unit"), str) and rule["unit"], f"Missing metric unit: {name}")
        baseline = checked_artifact(rule.get("baseline_samples"), root)
        sample_statistic(read_json(baseline), rule)
    return frozen


def full16_camera_plan(resource_root, scene_name, capture_path):
    scene_path = inside(resource_root, logical_path(scene_name))
    document = read_json(scene_path)
    references = {str(scene_path): artifact(scene_path)}
    base_shots, origin = camera_shots(scene_path, capture_path)
    require(capture_path.is_file(), "Explicit capture plan is required")
    references[str(capture_path)] = artifact(capture_path)
    placements = [item for item in document["objects"] if item.get("type") == "terrain"]
    require(len(placements) == 16 and len({item["name"] for item in placements}) == 16,
            "Full16 requires 16 unique terrain placements")
    tiles = []
    for placement in placements:
        require(placement.get("rotation", [0, 0, 0]) == [0, 0, 0]
                and placement.get("scale", [1, 1, 1]) == [1, 1, 1],
                "Camera adapter requires unrotated unit terrain placements")
        position = placement["position"]
        require(len(position) == 3, "Terrain position must have three components")
        for value in position:
            require(type(value) in (int, float) and math.isfinite(value), "Invalid terrain position")
        terrain_path = inside(resource_root, logical_path(placement["terrainPath"]))
        terrain = read_json(terrain_path)
        height_path = inside(resource_root, logical_path(terrain["heightmapPath"]))
        references[str(terrain_path)] = artifact(terrain_path)
        references[str(height_path)] = artifact(height_path)
        heights = png16(height_path)
        grid, margin = terrain["gridResolution"], terrain["heightmapMargin"]
        require(type(grid) is int and grid > 0 and type(margin) is int and margin >= 0,
                "Invalid terrain grid/margin")
        extent = grid + 2 * margin + 1
        require(len(heights) == extent and all(len(row) == extent for row in heights),
                "Heightmap differs from runtime margin contract")
        require(len(terrain["size"]) == 2, "Terrain size must have two components")
        for value in terrain["size"]:
            number(value, "terrain size", positive=True)
        number(terrain["heightScale"], "heightScale", positive=True)
        require(math.isfinite(terrain["heightOffset"]), "Invalid heightOffset")
        tiles.append({"name": placement["name"], "position": position,
                      "terrain": terrain, "heights": heights})
    columns = sorted({tile["position"][0] for tile in tiles})
    rows = sorted({tile["position"][2] for tile in tiles})
    require(len(columns) == len(rows) == 4, "Expected a 4x4 terrain grid")
    grid_tiles = {(columns.index(tile["position"][0]), rows.index(tile["position"][2])): tile
                  for tile in tiles}
    require(len(grid_tiles) == 16, "Duplicate terrain grid cell")
    for (column, row), tile in grid_tiles.items():
        for neighbor, axis in (((column + 1, row), 0), ((column, row + 1), 1)):
            if neighbor in grid_tiles:
                other = grid_tiles[neighbor]
                require(tile["terrain"]["size"] == other["terrain"]["size"],
                        "Camera adapter requires equal sized adjacent terrain")
                distance = other["position"][axis * 2] - tile["position"][axis * 2]
                require(math.isclose(distance, tile["terrain"]["size"][axis],
                                     rel_tol=1e-9, abs_tol=1e-5),
                        "Terrain grid contains a gap or overlap")
    near = next(shot for shot in base_shots if shot["name"] == "near")
    offset = [position - target for position, target in zip(near["position"], near["look_at"])]
    require(offset[1] > 0, "Near camera must be above target")
    shots = []

    def add(name, category, coordinates, cells):
        selected = [grid_tiles[cell] for cell in cells]
        heights = []
        for tile in selected:
            terrain = tile["terrain"]
            fractions = [(coordinates[axis] - tile["position"][axis * 2])
                         / terrain["size"][axis] + 0.5 for axis in (0, 1)]
            require(all(-1e-8 <= value <= 1 + 1e-8 for value in fractions),
                    "Camera target outside tile")
            sample_coordinates = [max(0, min(1, value)) * terrain["gridResolution"]
                                  + terrain["heightmapMargin"] for value in fractions]
            left, top = [math.floor(value) for value in sample_coordinates]
            right, bottom = [math.ceil(value) for value in sample_coordinates]
            blend_x, blend_z = [value - math.floor(value) for value in sample_coordinates]
            pixels = tile["heights"]
            sample = ((pixels[top][left] * (1 - blend_x) + pixels[top][right] * blend_x)
                      * (1 - blend_z)
                      + (pixels[bottom][left] * (1 - blend_x) + pixels[bottom][right] * blend_x)
                      * blend_z)
            heights.append(tile["position"][1] + sample / 65535 * terrain["heightScale"]
                           + terrain["heightOffset"])
        target = [coordinates[0], max(heights), coordinates[1]]
        shots.append({"name": name, "category": category,
                      "position": [value + delta for value, delta in zip(target, offset)],
                      "look_at": target, "surfaces": [tile["name"] for tile in selected],
                      "boundary_heights": heights,
                      "framing": "candidate; GPU visibility and active TES distance require evidence"})

    for row in range(4):
        for column in range(4):
            add(f"center_{column}_{row}", "center", [columns[column], rows[row]], [(column, row)])
            if column < 3:
                add(f"edge_x_{column}_{row}", "edge",
                    [(columns[column] + columns[column + 1]) / 2, rows[row]],
                    [(column, row), (column + 1, row)])
            if row < 3:
                add(f"edge_z_{column}_{row}", "edge",
                    [columns[column], (rows[row] + rows[row + 1]) / 2],
                    [(column, row), (column, row + 1)])
            if column < 3 and row < 3:
                add(f"junction_{column}_{row}", "junction",
                    [(columns[column] + columns[column + 1]) / 2, (rows[row] + rows[row + 1]) / 2],
                    [(column, row), (column + 1, row), (column, row + 1), (column + 1, row + 1)])
    for column, row in ((0, 0), (3, 0), (0, 3), (3, 3)):
        tile = grid_tiles[column, row]
        add(f"outer_{column}_{row}", "outer",
            [columns[column] + (-0.5 if column == 0 else 0.5) * tile["terrain"]["size"][0],
             rows[row] + (-0.5 if row == 0 else 0.5) * tile["terrain"]["size"][1]], [(column, row)])
    return {"source": origin, "overview": base_shots, "shots": shots,
            "counts": dict(Counter(shot["category"] for shot in shots)),
            "surface_ids": sorted("terrain|" + tile["name"] for tile in tiles),
            "inputs": list(references.values())}


def evidence_matrix(workers, weight_sources):
    return [{"id": f"{weight_source}/w{worker}/{name}", "case": name,
             "worker": worker, "weight_source": weight_source,
             "gates": ["observed_run_identity", *gates]}
            for weight_source in sorted(set(weight_sources))
            for worker in sorted(set(workers)) for name, gates in CASE_GATES.items()]


def build_plan(options):
    stage, package = options.stage.resolve(strict=True), options.package.resolve(strict=True)
    require(stage.is_dir() and package.is_dir() and stage != package
            and not stage.is_relative_to(package) and not package.is_relative_to(stage),
            "Stage and package must be existing disjoint directories")
    config_path = stage / "config/config.json"
    config = read_json(config_path)
    require(config["initScene"] == options.scene, "Stage scene mismatch")
    resource = Path(config["resourcePath"])
    resource = (stage / resource).resolve(strict=True)
    capture = options.capture_plan.resolve(strict=True) if options.capture_plan else (
        inside(resource, options.scene).parent / "capture_plan.json")
    cameras = full16_camera_plan(resource, options.scene, capture)
    files = {"stage_config": config_path, "stage_binary": stage / "bin/main_game.exe",
             "stage_flavor": stage / "bin/vl_build_flavor.json",
             "stage_shader_cache": stage / "shader/spv/shader-build-cache.json",
             "package_config": package / "config/config.json",
             "package_binary": package / "main_game.exe",
             "package_flavor": package / "vl_build_flavor.json",
             "package_manifest": package / "game-export-manifest.json",
             "package_shaders": package / "shader/runtime-shader.manifest.json",
             "package_shader_cache": package / "shader/spv/shader-build-cache.json",
             "p0": options.p0_freeze, "tool": Path(__file__)}
    files["exporter"] = (options.exporter.resolve() if options.exporter
                         else stage / "bin/vl_export_game.exe")
    package_variants = {}
    for value in getattr(options, "weight_package", None) or []:
        mode, separator, location = value.partition("=")
        require(separator and mode in WEIGHT_SOURCES and mode not in package_variants,
                "--weight-package requires unique WeightSource=directory")
        variant = Path(location).resolve(strict=True)
        variant_config = read_json(variant / "config/config.json")
        package_variants[mode] = {"path": str(variant), "scene": variant_config["initScene"]}
        for label, relative in (
                ("binary", "main_game.exe"), ("config", "config/config.json"),
                ("export", "game-export-manifest.json"), ("shader", "shader/runtime-shader.manifest.json"),
                ("cache", "shader/spv/shader-build-cache.json")):
            files[f"{mode}_{label}"] = variant / relative
    if package_variants:
        require(set(package_variants) == set(options.weight_source),
                "Every selected weight source needs its own package")
    for name in ("check_composited_fixture_camera", "run_composited_acceptance", "runtime_stress",
                 "rvt_quality_binary", "rvt_sample_evidence", "rvt_artifact_binding",
                 "rvt_run_conditions"):
        files[name] = Path(__file__).with_name(name + ".py")
    if options.adapter:
        files["adapter"] = options.adapter
    identities, blockers = {}, []
    for name, path in files.items():
        if path.is_file():
            identities[name] = artifact(path)
        else:
            blockers.append(f"Missing identity input {name}: {path}")
    frozen, summaries = None, {}
    try:
        adapter = load_adapter(options.adapter)
        if not options.p0_freeze.is_file():
            raise IncompleteEvidence("P0 freeze file is missing")
        frozen = validate_frozen(adapter.normalize_p0(options.p0_freeze), options.p0_freeze.resolve().parent)
        for mode in sorted(set(options.weight_source)):
            variant = package_variants.get(mode, {"path": str(package), "scene": options.scene})
            summary = adapter.inspect_package(Path(variant["path"]), variant["scene"], mode)
            require(summary.get("version") == 2, "Adapter did not establish V2")
            require(summary.get("surface_ids") == cameras["surface_ids"], "Package Full16 identity mismatch")
            require(summary.get("weight_source") == mode, "Package weightSource mismatch")
            for name in ("content_identity", "closure_sha256"):
                value = summary.get(name)
                require(isinstance(value, str) and len(value) == 64
                        and all(character in "0123456789abcdef" for character in value),
                        f"Missing package {name}")
            summaries[mode] = summary
    except (ValueError, OSError, KeyError, TypeError) as error:
        blockers.append(f"Required P0/Store adaptation: {error}")
    identity = {"files": identities, "scene": options.scene, "workers": sorted(set(options.worker)),
                "weight_sources": sorted(set(options.weight_source)),
                "camera_plan_sha256": canonical_hash(cameras),
                "frozen_sha256": canonical_hash(frozen), "packages": summaries}
    if package_variants:
        identity["package_variants"] = package_variants
    return {"kind": "rvt-delivery-evidence-plan", "version": 1,
            "status": "incomplete" if blockers else "planned", "acceptance_passed": False,
            "renderer_started": False, "identity": identity, "identity_sha256": canonical_hash(identity),
            "camera_plan": cameras, "matrix": evidence_matrix(options.worker, options.weight_source),
            "frozen": frozen, "blockers": blockers}


def sample_statistic(samples, rule):
    require(samples.get("sampling") == rule["sampling"]
            and samples.get("unit") == rule["unit"], "Sample scope/unit mismatch")
    require(samples.get("aggregation") == "raw", "Rolling/averaged timing is not raw evidence")
    values = samples.get("values")
    require(isinstance(values, list) and len(values) >= rule["min_samples"], "Missing raw samples")
    identities = samples.get("sample_ids")
    require(isinstance(identities, list) and len(identities) == len(values)
            and all(isinstance(identity, str) and identity for identity in identities)
            and len(set(identities)) == len(identities), "Missing/duplicate raw sample identities")
    for value in values:
        number(value, "sample")
    if rule["statistic"] == "max":
        return max(values)
    require(rule["sampling"] in ("per-frame", "gpu-timestamp"),
            "Percentiles cannot use wall-clock snapshots")
    from runtime_stress import percentile
    return percentile(values, float(rule["statistic"][1:]))


def verify_receipt(plan, receipt_path, adapter):
    if plan["blockers"]:
        raise IncompleteEvidence("; ".join(plan["blockers"]))
    require(plan["identity"]["workers"] == [1, 2]
            and plan["identity"]["weight_sources"] == sorted(WEIGHT_SOURCES),
            "Final acceptance requires both workers and both weight sources")
    receipt = read_json(receipt_path)
    require(receipt.get("identity_sha256") == plan["identity_sha256"], "Receipt identity mismatch")
    cases = receipt.get("cases")
    require(isinstance(cases, list), "Missing receipt cases")
    indexed = {case["id"]: case for case in cases}
    require(len(indexed) == len(cases) and set(indexed) == {case["id"] for case in plan["matrix"]},
            "Receipt matrix missing, duplicated or unexpected cases")
    for case in plan["matrix"]:
        evidence = indexed[case["id"]]
        not_applicable = evidence.get("status") == "not-applicable"
        require(evidence.get("status") == "passed" or (
            not_applicable and "native_p0" in plan["frozen"]
            and case["case"] in ("cache_deleted", "cache_corrupt")),
            f"Case not passed: {case['id']}")
        require(evidence.get("identity_sha256") == plan["identity_sha256"]
                and evidence.get("worker") == case["worker"]
                and evidence.get("weight_source") == case["weight_source"]
                and evidence.get("scene") == plan["identity"].get("package_variants", {}).get(
                    case["weight_source"], {"scene": plan["identity"]["scene"]})["scene"],
                f"Case execution identity mismatch: {case['id']}")
        entries = evidence.get("artifacts")
        require(isinstance(entries, list) and entries, f"Missing artifacts: {case['id']}")
        verified_paths = set()
        for entry in entries:
            path = checked_artifact(entry, receipt_path.resolve().parent)
            require(path.stat().st_size > 0, "Empty evidence artifact")
            verified_paths.add(str(path))
        for name, rule in plan["frozen"]["performance"].items():
            if case["case"] not in rule["cases"]:
                continue
            sample = evidence.get("metrics", {}).get(name)
            require(isinstance(sample, dict), f"Missing metric {name}: {case['id']}")
            raw_path = checked_artifact(sample.get("artifact"), receipt_path.resolve().parent)
            require(str(raw_path) in verified_paths, "Metric source is not a verified case artifact")
            require(read_json(raw_path) == sample["samples"], "Inline samples differ from raw artifact")
            require(sample_statistic(sample["samples"], rule) <= rule["maximum"],
                    f"Frozen performance gate failed: {case['id']} {name}")
        native_evidence = dict(evidence, artifact_root=str(receipt_path.resolve().parent))
        verdict = adapter.verify_case(case, native_evidence, plan["frozen"],
                                      plan["identity"]["packages"][case["weight_source"]])
        if not_applicable:
            require(verdict.get("status") == "not-applicable" and
                    verdict.get("reason") == "optional-disk-cache-disabled",
                    "Unsupported applicability exception")
            continue
        require(verdict.get("status") == "passed"
                and all(verdict.get("checks", {}).get(gate) is True for gate in case["gates"]),
                f"Unverified semantic gates: {case['id']}")
    return {"status": "passed", "identity_sha256": plan["identity_sha256"],
            "not_applicable_cases": [case["id"] for case in cases if case["status"] == "not-applicable"],
            "exercised_case_count": sum(case["status"] == "passed" for case in cases),
            "independent_review_scope": (
                "Independent reviewer must explicitly accept optional disk-cache exclusion; "
                "not-applicable cases were not exercised and are not passing cache mutation tests. "
                "Fresh physical residency and unvisited GPU production remain mandatory."),
            "receipt": artifact(receipt_path), "cases": len(cases), "renderer_started": False,
            "scope": "Evidence verified; independent stage acceptance still requires a non-implementer"}


def verify_installation(plan, package, timeout):
    number(timeout, "validator timeout", positive=True)
    entry = plan["identity"]["files"].get("exporter")
    if entry is None:
        raise IncompleteEvidence("Unified exporter validator is missing")
    executable = Path(entry["path"])
    require(executable.name.lower() in ("vl_export_game.exe", "vl_export_game"),
            "Installation verification requires vl_export_game")
    require(artifact(executable) == entry, "Exporter changed after identity binding")
    command = [str(executable), "--verify-package", str(package.resolve(strict=True))]
    result = subprocess.run(command, cwd=package, capture_output=True, timeout=timeout,
                            check=False, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    require(result.returncode == 0, f"Unified installation validator failed ({result.returncode}): {output}")
    require(artifact(executable) == entry, "Exporter changed during installation verification")
    return {"argv": command, "cwd": str(package.resolve()), "exit_code": result.returncode,
            "output": output, "exporter_sha256": entry["sha256"], "renderer_started": False}


def cli(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--weight-package", action="append",
                        help="WeightSource=installed-directory; repeat for separate two-mode packages")
    parser.add_argument("--scene", required=True)
    parser.add_argument("--worker", type=int, choices=(1, 2), nargs="+", required=True)
    parser.add_argument("--weight-source", "--weightSource", choices=WEIGHT_SOURCES,
                        nargs="+", required=True)
    parser.add_argument("--p0-freeze", type=Path, required=True)
    parser.add_argument("--adapter", type=Path)
    parser.add_argument("--exporter", type=Path)
    parser.add_argument("--validator-timeout", type=float, default=60)
    parser.add_argument("--capture-plan", type=Path)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--mode", choices=("plan", "verify"), default="plan")
    parser.add_argument("--output", type=Path)
    options = parser.parse_args(argv)
    logical_path(options.scene)
    if options.mode == "verify" and options.receipt is None:
        parser.error("--mode verify requires --receipt")
    if options.output:
        destination = options.output.resolve()
        require(not destination.exists(), "Refusing to overwrite evidence")
        for root in (options.stage, options.package):
            require(not destination.is_relative_to(root.resolve()), "Output cannot modify stage/package")
        require(destination not in (options.p0_freeze.resolve(),
                                    options.receipt.resolve() if options.receipt else None),
                "Output cannot overwrite an input")
    plan = build_plan(options)
    if options.mode == "verify":
        result = verify_receipt(plan, options.receipt, load_adapter(options.adapter))
        result["installation_validator"] = verify_installation(plan, options.package,
                                                               options.validator_timeout)
    else:
        result = plan
    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        write_json(options.output, result)
    print(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False))
    return 3 if result.get("status") == "incomplete" else 0


if __name__ == "__main__":
    try:
        sys.exit(cli())
    except IncompleteEvidence as error:
        print(f"incomplete: {error}", file=sys.stderr)
        sys.exit(3)
    except (ValueError, OSError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print(f"failed: {error}", file=sys.stderr)
        sys.exit(2)
