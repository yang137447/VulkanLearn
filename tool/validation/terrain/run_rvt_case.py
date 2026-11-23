"""Plan, launch, collect and independently validate one installed Game case."""

import argparse
from contextlib import contextmanager, ExitStack
import json
import os
from pathlib import Path
import subprocess
import sys

from accept_game_delivery import negative_payload
from build_composited_fixture import read_json, reference_path
from runtime_stress import sampled_process
from rvt_delivery import CASE_GATES, IncompleteEvidence, artifact, require
from rvt_native_adapter import inspect_package, normalize_p0, verify_case
from rvt_p0_analyze import extract
from rvt_artifact_binding import bind_artifact, event_artifacts, MAX_JSON_BYTES
from rvt_run_conditions import process_conditions, quiet_host_audit, analyze_complete_raw


def schedule(name, capture, scenario):
    commands = []
    shots = capture["shots"]
    if scenario is not None and scenario.get("commands"):
        return scenario["commands"]
    if name in ("production_failure", "over_budget", "editor_refresh", "world_replace", "offline_game"):
        require(scenario is not None and scenario.get("commands"),
                f"{name} requires explicit native scenario commands; no guessed test hook")
        return scenario["commands"]
    if name == "tour":
        selected = [shot for shot in shots if shot.get("category") == "center"]
    elif name == "seams":
        selected = [shot for shot in shots if shot.get("category") in ("edge", "junction", "outer")]
    elif name == "teleport":
        selected = [shot for shot in shots if shot["name"] in ("near", "tile00_near", "cross_tile_corner", "far")]
    elif name == "tes":
        selected = [shot for shot in shots if shot["name"] == "near"]
    else:
        selected = shots[:1]
    for shot in selected:
        commands.append("camera pose " + " ".join(str(value) for value in shot["position"] + shot["look_at"]))
        commands.extend(["terrainvt stats"] * 4)
    return commands


def fault_target(package, case):
    resources = package / "resources"
    scene = read_json(reference_path(resources, read_json(package / "config/config.json")["initScene"]))
    placement = next(row for row in scene["objects"] if row.get("type") == "terrain")
    terrain_path = reference_path(resources, placement["terrainPath"])
    terrain = read_json(terrain_path)
    manifest = reference_path(resources, terrain["compositedVT"]["packagePath"])
    document = read_json(manifest)
    if case == "v1_reject":
        return manifest, "wrong-version", 0
    if case == "legacy_switch_reject":
        return terrain_path, "tr-resident-pages", 0
    if case == "root_corrupt":
        root = document["roots"][0]
        page = next(row for row in document["pages"]
                    if all(row[field] == root[field] for field in ("surfaceId", "mip", "x", "y")))
        return reference_path(manifest.parent, document["package"]), "corrupt-root", page["offset"]
    if case == "input_corrupt":
        # V1 发布把输入写成 {layerName: objectId} 字典；V2 发布写成 objectId 数组。
        # 两种真实 schema 都要能定位到一个实际生产输入对象，不能按旧字段名直接下标。
        inputs = document["production"]["surfaces"][0]["inputs"]
        source = inputs.get("layerAlbedo") if isinstance(inputs, dict) else inputs[0]
        entry = next(row for row in document["production"]["objects"] if row["objectId"] == source)
        return reference_path(manifest.parent, entry["path"]), "corrupt-production-input", 0
    return None


@contextmanager
def mutation(target, output):
    if target is None:
        yield None
        return
    path, kind, offset = target
    original = path.read_bytes()
    backup = output / "fault-original.bin"
    backup.write_bytes(original)
    record = {"path": str(path), "kind": kind, "offset": offset, "original": artifact(backup)}
    try:
        path.write_bytes(negative_payload(kind, original, offset))
        record["mutated"] = artifact(path)
        yield record
    finally:
        path.write_bytes(original)
        require(path.read_bytes() == original, "Failed restoring exact package payload")


def run(options):
    package = options.package.resolve(strict=True)
    config = read_json(package / "config/config.json")
    require(config["resourcePath"] == "resources", "Independent installed Game required")
    output = options.output.resolve()
    require(not output.exists() and not output.is_relative_to(package) and
            not package.is_relative_to(output), "Evidence needs fresh directory outside install")
    capture = read_json(options.capture_plan)
    scenario = read_json(options.scenario) if options.scenario else None
    environment, conditions = process_conditions(
        package, output, config, options.profile, options.worker, options.quality_mode)
    commands = schedule(options.case, capture, scenario)
    executable = package / "main_game.exe"
    argv = [str(executable), "--initial-scene", config["initScene"],
            "--worker-thread-count", str(options.worker), "--frame-limit", "30",
            "--present-mode", "immediate", "--no-dev-ui",
            "--validation" if conditions["validation"] else "--no-validation",
            "--exit-after-frames", str(options.frames),
            "--console-command-delay", "1", "--perf-jsonl", str(output / "runtime.perf.jsonl")]
    for command in commands:
        require(isinstance(command, str) and "\n" not in command, "Invalid native console command")
        argv.extend(["--console-command", command])
    if scenario:
        for argument in scenario.get("launchArguments", []):
            require(isinstance(argument, str), "Scenario arguments must be strings")
            require(argument.split("=", 1)[0] not in ("--worker-thread-count", "--initial-scene", "--exit-after-frames",
                                     "--perf-jsonl", "--console-command", "--console-command-delay",
                                     "--frame-limit", "--present-mode", "--validation", "--no-validation"),
                    "Scenario cannot override bound execution identity or collection settings")
            argv.append(argument)
    require(len(subprocess.list2cmdline(argv)) < 32767, "Case schedule exceeds Windows command bound")
    target = fault_target(package, options.case)
    plan = {"case": options.case, "worker": options.worker, "weightSource": options.weight_source,
            "scene": config["initScene"], "argv": argv, "cwd": str(package),
            "fault": {"path": str(target[0]), "kind": target[1], "offset": target[2]} if target else None,
            "processConditions": conditions,
            "rendererStarted": False, "acceptancePassed": False}
    if not options.execute:
        print(json.dumps(plan, indent=2))
        return 0
    frozen = normalize_p0(options.p0_freeze)
    summary = inspect_package(package, config["initScene"], options.weight_source)
    execution_inputs = {name: artifact(path) for name, path in (
        ("binary", executable), ("shader_manifest", package / "shader/runtime-shader.manifest.json"),
        ("shader_cache", package / "shader/spv/shader-build-cache.json"),
        ("frozen", package / "config/terrainRvtP0Frozen.json"), ("capture_plan", options.capture_plan),
        ("runner", Path(__file__)))}
    if options.scenario:
        execution_inputs["scenario"] = artifact(options.scenario)
    if options.owned_package_receipt:
        execution_inputs["owned_package_receipt"] = artifact(options.owned_package_receipt)
    if options.case == "offline_game":
        execution_inputs["acl_script"] = artifact(Path(__file__).with_name("readonly_game_acl.ps1"))
        execution_inputs["acl_lifecycle"] = artifact(Path(__file__).with_name("rvt_readonly_install.py"))
    for name in ("rvt_quality_binary", "rvt_sample_evidence", "rvt_artifact_binding", "rvt_native_adapter",
                 "rvt_run_conditions"):
        execution_inputs[name] = artifact(Path(__file__).with_name(name + ".py"))
    if options.case in ("cache_deleted", "cache_corrupt"):
        require(scenario and scenario.get("diskCacheMode") == "disabled",
                "Bounded disk-cache mutation requires actual cache owner integration; no fabricated mutation")
    output.mkdir(parents=True)
    (output / "quality").mkdir()
    (output / "plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
    (output / "process-conditions.json").write_text(json.dumps(conditions, indent=2), encoding="utf-8")
    artifacts = []
    collected = {}

    def collect(path, role):
        entry = artifact(path)
        entry.update(path=path.relative_to(output).as_posix(), role=role)
        if entry["path"] in collected:
            require(collected[entry["path"]]["sha256"] == entry["sha256"],
                    "Collected artifact changed during run")
            return collected[entry["path"]]
        collected[entry["path"]] = entry
        artifacts.append(entry)
        return entry

    with mutation(target, output) as fault:
        if fault:
            (output / "fault.json").write_text(json.dumps(fault, indent=2), encoding="utf-8")
            collect(output / "fault.json", "fault")
            collect(output / "fault-original.bin", "fault_backup")
            verification = subprocess.run([os.environ["VULKANLEARN_RVT_VALIDATOR"], "--verify-package", str(package)],
                                          cwd=package, capture_output=True, timeout=300, check=False,
                                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            (output / "installation.log").write_bytes(verification.stdout + verification.stderr)
            install = {"event": "installation", "tool": "vl_export_game", "exitCode": verification.returncode,
                       "argv": verification.args, "logArtifact": collect(output / "installation.log", "installation_log")}
            (output / "installation.json").write_text(json.dumps(install), encoding="utf-8")
            collect(output / "installation.json", "runner_event")
        with ExitStack() as install_context:
            if options.profile == "formal-performance":
                install_context.enter_context(quiet_host_audit(output / "host-audit.jsonl"))
            if options.case == "offline_game":
                from rvt_readonly_install import readonly_package
                require(options.owned_package_receipt is not None,
                        "Offline read-only run requires actual owned package export receipt")
                project = Path(__file__).resolve().parents[3]
                probe = install_context.enter_context(readonly_package(
                    package, options.owned_package_receipt, output, project,
                    project.parent / "VukanLearnResources"))
                (output / "readonly-probe.json").write_text(json.dumps(probe, indent=2), encoding="utf-8")
            process, text = sampled_process(argv, package, output / "runtime.log", options.timeout, 500,
                                            env=environment)
        (output / "process.json").write_text(json.dumps(process, indent=2), encoding="utf-8")
        collect(output / "process.json", "process")
        collect(output / "runtime.log", "runtime_log")
        collect(output / "process-conditions.json", "process_conditions")
        for path in output.glob("*.jsonl"):
            collect(path, "raw_samples")
        for name in ("acl-command.json", "readonly-probe.json", "package-acl-backup.json", "acl-protect.stdout",
                     "acl-protect.stderr", "acl-restore.stdout", "acl-restore.stderr"):
            if (output / name).is_file() and (output / name).stat().st_size:
                collect(output / name, "readonly_installation")
        events, errors = extract(text, "[RVT_EVIDENCE] ")
        require(not errors, "Malformed runtime telemetry")
        raw_analysis = analyze_complete_raw(text, events)
        (output / "raw-analysis.json").write_text(json.dumps(raw_analysis, indent=2), encoding="utf-8")
        collect(output / "raw-analysis.json", "raw_analysis")
        if (output / "host-audit.summary.json").is_file():
            collect(output / "host-audit.summary.json", "host_audit")
        for event in events:
            for role, reference in event_artifacts(event):
                path = bind_artifact(output, reference, MAX_JSON_BYTES)
                collect(path, role)
        for name, entry in execution_inputs.items():
            require(artifact(Path(entry["path"])) == entry, f"Execution input drifted: {name}")
        (output / "execution-inputs.json").write_text(json.dumps(execution_inputs, indent=2), encoding="utf-8")
        collect(output / "execution-inputs.json", "execution_identity")
        evidence = {"status": "not-applicable" if options.case in ("cache_deleted", "cache_corrupt") else "collected",
                    "worker": options.worker, "weight_source": options.weight_source, "scene": config["initScene"],
                    "artifact_root": str(output), "artifacts": artifacts}
        case = {"id": f"{options.weight_source}/w{options.worker}/{options.case}", "case": options.case,
                "worker": options.worker, "weight_source": options.weight_source,
                "gates": ["observed_run_identity", *CASE_GATES[options.case]]}
        try:
            verdict = verify_case(case, evidence, frozen, summary)
            if options.profile == "formal-performance":
                audit = read_json(output / "host-audit.summary.json")
                require(audit["complete"] and audit["samples"] > 0 and not audit["busy"] and not audit["errors"],
                        "Formal performance host audit had compiler interference or missing samples")
                require(raw_analysis["status"] == "raw-collected",
                        "Formal raw streams/phases/quality proof incomplete")
                verdict["status"] = "collected-not-formal-accepted"
                verdict["performanceScope"] = raw_analysis
        except (IncompleteEvidence, ValueError, KeyError, TypeError) as error:
            verdict = {"status": "incomplete", "reason": str(error)}
        result = {"case": case, "evidence": evidence, "verdict": verdict,
                  "rendererStarted": True, "acceptancePassed": False,
                  "executionInputs": execution_inputs,
                  "scope": "One real case collection; frozen performance matrix and independent review remain required"}
        (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result["verdict"], indent=2))
    return 0 if verdict["status"] in ("passed", "not-applicable") else 3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--capture-plan", type=Path, required=True)
    parser.add_argument("--p0-freeze", type=Path, required=True)
    parser.add_argument("--worker", type=int, choices=(1, 2), required=True)
    parser.add_argument("--weight-source", choices=("CoverageAtlas", "LocalLayerArray"), required=True)
    parser.add_argument("--case", choices=tuple(CASE_GATES), required=True)
    parser.add_argument("--scenario", type=Path)
    parser.add_argument("--profile", choices=("correctness", "formal-performance"), default="correctness")
    parser.add_argument("--quality-mode", choices=("capture-4hz", "paired-steady"), default="capture-4hz")
    parser.add_argument("--owned-package-receipt", type=Path,
                        help="Actual new package export receipt; required for offline NTFS ACL run")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=1400)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--execute", action="store_true")
    options = parser.parse_args()
    require(options.frames >= 1000 and options.timeout > 0, "Need bounded run and >=1000 raw frames")
    return run(options)


if __name__ == "__main__":
    sys.exit(main())
