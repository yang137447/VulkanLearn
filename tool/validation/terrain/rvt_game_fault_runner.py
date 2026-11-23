"""Bounded real installed-Game fault execution; planning never starts a process."""

from contextlib import ExitStack, contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from types import SimpleNamespace

from rvt_native_adapter import FaultEvidence, fault_frame_witness
from rvt_full_tour import (PendingProducerArtifact, capture_metadata, decode_capture,
                           inject_command, camera_command, camera_matches)
from rvt_quality_binary import OWNER_OFFSET, SURFACE_OFFSET, join


CASES = ("statusNonzero", "submitFail", "slotReuse", "staleRevision", "budgetReject",
         "candidate_image", "candidate_raw_kind3", "root_payload", "v1_version",
         "legacy_material_switch")
ALIASES = {"producer_status": "statusNonzero", "submit_failure": "submitFail",
           "late_revision": "staleRevision", "max_budget": "budgetReject",
           "slot_reuse": "slotReuse", "candidate_retention": "candidate_image",
           "production_image": "candidate_image", "production_raw_kind3": "candidate_raw_kind3"}


def matrix():
    import rvt_fault_matrix
    return rvt_fault_matrix


def require(condition, message):
    matrix().require(condition, message)


def raw_production(text):
    records = []
    for line in text.splitlines():
        marker = "[RVT_PRODUCTION_RAW] "
        if marker in line:
            from rvt_full_tour import strict_json_bytes
            records.append(strict_json_bytes(line.split(marker, 1)[1].encode("utf-8")))
    return records


def quality_snapshot(evidence, output, surfaces, surface_id, after_sequence=0, shot=None, min_epoch=0):
    states, completed = {}, {}
    for row in evidence.records:
        payload = row["payload"]
        if row["eventClass"] == "RVT_QUALITY_STATE":
            key = (payload["worldGeneration"], str(payload["poseId"]), payload["submissionEpoch"])
            states[key] = payload
        if row["eventClass"] == "RVT_QUALITY_EVENT" and payload.get("event") == "capture_completed":
            key = (payload["worldGeneration"], str(payload["poseId"]), payload["submissionEpoch"])
            completed[key] = row
    stream = SimpleNamespace(output=output, dedicated=True, states=states, completed=completed)
    for row in reversed(evidence.find("RVT_QUALITY_CAPTURE")):
        if row["sequence"] <= after_sequence:
            continue
        try:
            decoded = decode_capture(row["payload"], stream, surfaces)
            metadata, _, _, data = capture_metadata(row["payload"], stream)
        except PendingProducerArtifact:
            continue
        if decoded["counts"]["visibleSamples"] == 0 or (shot and not camera_matches(shot, metadata)):
            continue
        if metadata["submissionEpoch"] <= min_epoch:
            continue
        if after_sequence and not any(row["sequence"] > after_sequence and
            row["eventClass"] == "CPU_RAW" and all(row["payload"].get(key) == value for key, value in (
                ("renderFrame", metadata["frame"]), ("worldGeneration", metadata["worldGeneration"]),
                ("submitEpoch", metadata["submissionEpoch"]), ("acquiredReady", True)))
            for row in evidence.records):
            continue
        index = metadata["surfaceIds"].index(surface_id)
        fields = struct.unpack_from("<8I", data, SURFACE_OFFSET + index * 64)
        world, revision = join(*fields[4:6]), join(*fields[6:8])
        require(world == metadata["worldGeneration"], "Quality surface/World identity differs")
        owners = []
        for slot in range(512):
            offset = OWNER_OFFSET + slot * 64
            owner = struct.unpack_from("<8I", data, offset)
            if owner[0] and owner[1] == index and owner[4:8] == fields[4:8]:
                owners.append({"slot": slot, "mip": owner[2], "x": owner[3] & 65535,
                               "y": owner[3] >> 16, "contentIdentity": data[offset + 32:offset + 64].hex()})
        return {"sequence": row["sequence"], "worldGeneration": world, "revision": revision,
                "surfaceId": surface_id, "contentIdentity": data[
                    SURFACE_OFFSET + index * 64 + 32:SURFACE_OFFSET + index * 64 + 64].hex(),
                "owners": owners, "quality": decoded}
    return None


def request_for(case, snapshot, target, fault_id, options):
    request = {"kind": case, "faultId": fault_id, "worldGeneration": snapshot["worldGeneration"]}
    if case != "budgetReject":
        request.update(surfaceId=snapshot["surfaceId"], revision=snapshot["revision"], **target)
    if case == "statusNonzero":
        request["status"] = options.status
    elif case == "submitFail":
        request["submitResult"] = options.submit_result
    elif case == "staleRevision":
        request["replacementRevision"] = options.replacement_revision or snapshot["revision"] + 1
        require(request["replacementRevision"] > snapshot["revision"], "Stale target must be a newer revision")
    elif case == "budgetReject":
        request["testAdmissionLimitBytes"] = options.admission_limit
    elif case == "slotReuse":
        require(options.slot is not None and options.slot_publish_epoch is not None,
                "slotReuse needs explicit --slot and --slot-publish-epoch expectations; never infer publish epoch")
        request.update(slot=options.slot, writeEpoch=options.slot_publish_epoch)
    return request


def control_event(evidence, fault_id):
    rows = evidence.find("RVT_FAULT_CONTROL", fault_id=fault_id)
    if not rows:
        return None
    require(len(rows) == 1, "Repeated control response for one faultId")
    require(rows[0]["payload"]["result"] == "queued",
            "Native fault request rejected: " + json.dumps(rows[0]["payload"]))
    return rows[0]


def fault_terminal(case, evidence, request):
    rows = evidence.find(fault_id=request["faultId"])
    unique = ("fault-injection", "production-failure", "submit-fault-consumed", "submit-abort-observed",
              "actual-completion-held", "actual-stale-completion-delivered", "candidate-budget-admission",
              "slot-watch-armed", "slot-watch-replacement-observed")
    for name in unique:
        require(sum(row["payload"].get("event") == name for row in rows) <= 1,
                "Duplicate native one-shot event: " + name)
    for row in rows:
        payload = row["payload"]
        if "worldGeneration" in payload:
            require(payload["worldGeneration"] == request["worldGeneration"], "Fault event World differs")
        if case in ("statusNonzero", "submitFail") and row["eventClass"] == "RVT_EVIDENCE":
            for field in ("surfaceId", "mip", "x", "y", "revision"):
                require(payload.get(field) == request[field], "Fault event page/revision differs: " + field)
    events = {row["payload"].get("event"): row for row in rows}
    if case == "statusNonzero":
        injected, failed = events.get("fault-injection"), events.get("production-failure")
        if not injected or not failed:
            return None
        raw, result = injected["payload"], failed["payload"]
        require(raw["originalStatus"] == 0 and raw["injectedStatus"] == request["status"] and
                result["status"] == request["status"] and result["publishedChannels"] == 0 and
                result["fenceCompletedEpoch"] >= result["writeEpoch"] > 0 and
                0 < result["attempts"] <= result["maxAttempts"] and result["backoffTicks"] > 0,
                "Status failure did not follow actual nonpublication/retry/backoff")
        return failed
    if case == "submitFail":
        consumed, aborted = events.get("submit-fault-consumed"), events.get("submit-abort-observed")
        if not consumed or not aborted:
            return None
        raw, result = consumed["payload"], aborted["payload"]
        require(raw["abortTicket"] == result["abortTicket"] and raw["contentIdentity"] == result["contentIdentity"]
                and raw["submissionEpoch"] == result["submissionEpoch"] and
                result["rawSubmitResult"] is None and result["sceneSubmitCalls"] == 0 and
                result["publishedChannels"] == 0 and result["recordedTasks"] == result["abortedTasks"] > 0 and
                result["activeGenerationBefore"] == result["activeGenerationAfter"] == request["worldGeneration"],
                "Actual Abort ticket/transaction/world observation differs")
        return aborted
    if case == "staleRevision":
        held, delivered = events.get("actual-completion-held"), events.get("actual-stale-completion-delivered")
        if not held or not delivered:
            return None
        before, after = held["payload"], delivered["payload"]
        require(before["writeEpoch"] == after["writeEpoch"] and before["reservation"] == after["reservation"] and
                before["contentIdentity"] == after["oldContentIdentity"] and
                after["oldRevision"] == request["revision"] and after["normalRuntimeAccepted"] is False and
                after["cancelledOrTimedOut"] is False and after["completedEpoch"] >= after["writeEpoch"],
                "Stale task was fabricated/changed/accepted or cancelled")
        commits = evidence.find("RVT_WORLD_CANDIDATE")
        require(any(held["sequence"] < row["sequence"] < delivered["sequence"] and
                    row["payload"]["worldGeneration"] == request["worldGeneration"] and
                    row["payload"]["revision"] >= request["replacementRevision"] for row in commits),
                "Old task delivery lacks an actual newer joint commit")
        return delivered
    if case == "budgetReject":
        rejected = events.get("candidate-budget-admission")
        if not rejected:
            return None
        actual = rejected["payload"]
        require(actual["validStrictLowerLimit"] is True and actual["rejected"] is True and
                actual["testLimit"] == request["testAdmissionLimitBytes"] and
                0 < actual["testLimit"] < actual["actualFrozenLimit"] and
                actual["actualDemand"] > actual["testLimit"], "Candidate rejection did not use lower actual admission")
        return rejected
    if case == "slotReuse":
        for name in ("slot-watch-identity-rejected", "slot-watch-admission-rejected", "slot-watch-timeout",
                     "slot-watch-cancelled", "slot-watch-owner-teardown"):
            require(name not in events, "Slot watch did not finish: " + name)
        armed, replacement = events.get("slot-watch-armed"), events.get("slot-watch-replacement-observed")
        if not armed:
            return None
        if not replacement:
            return None
        actual = replacement["payload"]["observation"]
        original, current = actual["original"], actual["current"]
        require(armed["sequence"] < replacement["sequence"], "Slot replacement predates actual arm")
        token = original["publishedToken"]
        require(all(token.get(key) == request[key] for key in
                    ("worldGeneration", "surfaceId", "mip", "x", "y", "revision")),
                "Original slot token does not match explicit request")
        require(armed["payload"]["observation"]["original"] == original,
                "Slot watch changed its original native snapshot")
        require(actual["releasedAtBoundary"] is True and actual["replacementObserved"] is True and
                actual["replacementStillPresent"] is True and actual["retirementObserved"] is True and
                actual["blockedByEpoch"] + actual["blockedByOwner"] > 0 and
                actual["releaseCompletedEpoch"] >= max(actual["releaseRetireEpoch"], actual["releaseReferenceEpoch"])
                and current["reservation"] == actual["replacementReservation"] != original["reservation"] and
                current["reservedToken"] != original["publishedToken"] and
                original["slot"] == current["slot"] == request["slot"] and
                original["publishEpoch"] == request["writeEpoch"], "Actual slot reuse boundary proof differs")
        return replacement
    return None


@contextmanager
def restore_shader_outputs(private, output):
    root = (private / "shader/spv").resolve()
    require(root.is_relative_to(private.resolve()) and root.is_dir(), "Missing private shader cache")
    backup = output / "shader-output-before"
    shutil.copytree(root, backup, copy_function=shutil.copyfile)
    baseline = matrix().inventory(root)
    try:
        yield
    finally:
        matrix().regular_tree(root)
        for path in root.rglob("*"):
            if path.is_file() and path.relative_to(root).as_posix() not in baseline:
                require(path.resolve().is_relative_to(root), "Shader restoration escaped private copy")
                path.unlink()
        for name in baseline:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(backup / name, target)
        require(matrix().inventory(root) == baseline, "Private shader outputs failed exact restoration")


class Game:
    def __init__(self, private, output, config, options):
        self.private, self.output, self.options = private, output, options
        self.evidence = FaultEvidence(output / "runtime-events.jsonl")
        self.start = time.monotonic()
        self.process = None
        self.request = None
        self.stdout = (output / "stdout.log").open("wb")
        self.stderr = (output / "stderr.log").open("wb")
        from rvt_run_conditions import process_conditions
        env, conditions = process_conditions(private, output, config, "correctness", options.worker, "capture-4hz")
        (output / "quality").mkdir()
        env["VULKANLEARN_RUNTIME_EVIDENCE"] = str(self.evidence.path)
        env["VULKANLEARN_RUNTIME_CACHE_ROOT"] = str(output / "cache")
        conditions["environmentOverrides"].update({key: env[key] for key in
            ("VULKANLEARN_RUNTIME_EVIDENCE", "VULKANLEARN_RUNTIME_CACHE_ROOT")})
        self.argv = [str(private / "main_game.exe"), "--initial-scene", config["initScene"],
                     "--worker-thread-count", str(options.worker), "--frame-limit", "30",
                     "--present-mode", "immediate", "--no-dev-ui", "--validation", "--gpu-stats",
                     "--exit-after-frames", str(options.frames)]
        matrix().save(output / "process-conditions.json", conditions)
        self.env = env

    def __enter__(self):
        require(os.name == "nt", "Real conio Game injection requires Windows")
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        try:
            self.process = subprocess.Popen(self.argv, cwd=self.output, env=self.env,
                stdout=self.stdout, stderr=self.stderr, creationflags=subprocess.CREATE_NEW_CONSOLE,
                startupinfo=startup)
        except OSError as error:
            self.stdout.close()
            self.stderr.close()
            matrix().save(self.output / "process.json", {"argv": self.argv, "cwd": str(self.output),
                "ProcessExit": None, "launchError": str(error)})
            raise
        matrix().save(self.output / "process-start.json",
                      {"pid": self.process.pid, "argv": self.argv, "cwd": str(self.output)})
        return self

    def send(self, command):
        require(self.process.poll() is None, "Game exited before actual command injection")
        inject_command(self.process.pid, command, self.output)

    def wait(self, predicate, label, timeout=None):
        deadline = time.monotonic() + (timeout or self.options.phase_timeout)
        while True:
            self.evidence.poll()
            value = predicate()
            if value:
                return value
            require(self.process.poll() is None, "Game exited awaiting " + label)
            require(time.monotonic() < deadline and time.monotonic() - self.start < self.options.timeout,
                    "Timeout awaiting actual " + label)
            time.sleep(0.03)

    def text(self):
        return "\n".join((self.output / name).read_text(encoding="utf-8", errors="replace")
                         for name in ("stdout.log", "stderr.log"))

    def __exit__(self, kind, error, traceback):
        shutdown_error = None
        footer_error = None
        try:
            if self.process and self.process.poll() is None:
                if kind and self.request:
                    self.send("rvtfault " + json.dumps({"kind": "cancel", "faultId": self.request["faultId"]}))
                self.send("quit")
                self.process.wait(timeout=15)
        except (ValueError, OSError, subprocess.TimeoutExpired) as failure:
            shutdown_error = str(failure)
        finally:
            if self.process and self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=10)
            self.stdout.close()
            self.stderr.close()
            try:
                self.evidence.poll(final=True)
            except (ValueError, OSError, KeyError, TypeError) as failure:
                footer_error = str(failure)
            matrix().save(self.output / "evidence-validation.json", {
                "valid": footer_error is None, "error": footer_error,
                "records": len(self.evidence.records), "bytes": self.evidence.bytes})
            record = {"argv": self.argv, "cwd": str(self.output),
                      "pid": self.process.pid if self.process else None,
                      "ProcessExit": self.process.returncode if self.process else None,
                      "timedOut": bool(error and "Timeout awaiting" in str(error)),
                      "error": str(error) if error else shutdown_error or footer_error,
                      "elapsedSeconds": time.monotonic() - self.start}
            matrix().save(self.output / "process.json", record)
        if kind is None:
            require(not shutdown_error and record["ProcessExit"] == 0, "Game crash/nonzero/unclean shutdown")
            require(footer_error is None, "Dedicated evidence incomplete: " + str(footer_error))
            require("EngineLoop teardown complete" in self.text(), "Missing native clean teardown")
            require(not re.search(r"VUID-|Validation Error|ErrorDeviceLost|VK_ERROR_DEVICE_LOST|Unknown command",
                                  self.text(), re.I), "Native validation/device/command error")


def run_case(private, output, options, case):
    config, manifest, world = matrix().layout(private)
    surfaces = {row["surfaceId"]: row for row in world["surfaces"]}
    shot_plan = matrix().read_json(options.capture_plan)
    shots = {row["name"]: row for row in shot_plan["shots"]}
    baseline_shot, target_shot = shots[options.baseline_camera], shots[options.camera]
    target = {"mip": options.mip, "x": options.page_x, "y": options.page_y}
    patches = {row["case"]: row for row in matrix().mutations(private)[1]}
    aliases = {"candidate_image": "production_image", "candidate_raw_kind3": "production_raw_kind3"}
    patch_name = aliases.get(case, case)
    fault_patch = patches.get(patch_name)
    if case in ("candidate_image", "candidate_raw_kind3", "root_payload", "v1_version", "legacy_material_switch"):
        require(fault_patch is not None, "Required file/object case missing; not N/A")
        fault_patch["mutatedSha256"] = matrix().virtual_hash(private / fault_patch["path"], fault_patch)
    proof = {"case": case, "acceptancePassed": False}
    if case == "root_payload":
        inputs = {row["path"]: matrix().identity(matrix().contained(manifest.parent, row["path"]))
                  for row in world["production"]["objects"]}
        matrix().save(output / "required-inputs-before.json", inputs)
    with ExitStack() as cleanup:
        if case == "root_payload":
            cleanup.enter_context(matrix().patched(private, fault_patch, output))
            require(all(matrix().identity(matrix().contained(manifest.parent, name)) == recorded
                        for name, recorded in inputs.items()), "Root test changed a required production input")
            proof["requiredProductionInputsUnchanged"] = True
            proof["installation"] = verify_mutated(private, output, options)
        with Game(private, output, config, options) as game:
            game.send(camera_command(baseline_shot))
            snapshot = game.wait(lambda: quality_snapshot(game.evidence, output, surfaces,
                options.surface_id, shot=baseline_shot), "initial actual Quality ABI", options.timeout)
            before = game.wait(lambda: fault_frame_witness(game.evidence.records,
                snapshot["worldGeneration"]), "initial actual frame/fence")
            if options.world_generation is not None:
                require(snapshot["worldGeneration"] == options.world_generation, "Requested/live World differs")
            if options.revision is not None:
                require(snapshot["revision"] == options.revision, "Requested/live revision differs")
            proof.update(before=snapshot, beforeFrame=before)
            if case == "root_payload":
                root = fault_patch["page"]
                production = game.wait(lambda: next((row for row in raw_production(game.text())
                    if row["mip"] == root["mip"] and row["x"] == root["x"] and row["y"] == root["y"]
                    and row["tokenContentHash"] == snapshot["contentIdentity"] and row["status"] == 0 and
                    row["completedEpoch"] >= row["submissionEpoch"]), None), "actual rebuilt root production task")
                proof["production"] = production
            elif fault_patch:
                with matrix().patched(private, fault_patch, output):
                    proof["installation"] = verify_mutated(private, output, options)
                    cutoff = len(game.evidence.records)
                    game.send("loadworld " + config["initScene"])
                    rejection = game.wait(lambda: next((line for line in game.text().splitlines()
                        if "LoadWorld transaction failed" in line), None), "actual candidate load rejection")
                    require("integrity mismatch" in game.text() if case.startswith("candidate_") else
                            re.search(r"version|V1|legacy|u_terrainLayerMode|unsupported", game.text(), re.I),
                            "Candidate reject lacks original native input/version/switch diagnostic")
                    proof["rejectionLog"] = rejection
                    proof["after"] = game.wait(lambda: quality_snapshot(game.evidence, output, surfaces,
                        options.surface_id, cutoff), "old World actual capture after rejection")
                    require(proof["after"]["worldGeneration"] == snapshot["worldGeneration"],
                            "Candidate rejection changed active World")
                    proof["afterFrame"] = game.wait(lambda: fault_frame_witness(game.evidence.records,
                        snapshot["worldGeneration"], before["gpu"]["payload"]["submitEpoch"], cutoff),
                        "old World subsequent real frame/fence")
            else:
                if case == "slotReuse":
                    cutoff = len(game.evidence.records)
                    game.send(camera_command(target_shot))
                    snapshot = game.wait(lambda: quality_snapshot(game.evidence, output, surfaces,
                        options.surface_id, cutoff, target_shot), "actual resident slot target")
                    require(any(owner["slot"] == options.slot and all(owner[key] == target[key]
                        for key in ("mip", "x", "y")) for owner in snapshot["owners"]),
                        "Requested slot/page is not present in actual raw owner table")
                request = request_for(case, snapshot, target, output.name, options)
                game.request = request
                matrix().save(output / "request.json", request)
                game.send("rvtfault " + json.dumps(request, separators=(",", ":")))
                game.wait(lambda: control_event(game.evidence, request["faultId"]), "queued native control")
                if case in ("statusNonzero", "submitFail", "staleRevision"):
                    game.send(camera_command(target_shot))
                if case == "staleRevision":
                    game.wait(lambda: game.evidence.find(event="actual-completion-held",
                        fault_id=request["faultId"]), "real completed task hold")
                if case in ("staleRevision", "slotReuse", "budgetReject"):
                    if case == "slotReuse":
                        game.wait(lambda: game.evidence.find(event="slot-watch-armed",
                            fault_id=request["faultId"]), "actual charged slot watch arm")
                    cleanup.enter_context(restore_shader_outputs(private, output))
                    shader = matrix().contained(private, options.producer_source)
                    require(shader.stat().st_size <= matrix().JSON_LIMIT, "Producer source exceeds patch bound")
                    original = shader.read_bytes()
                    source_patch = matrix().patch_record(private, shader, 0, original,
                        original + b"\n// explicit private fault-runner producer reload\n", "producer_reload")
                    source_patch["mutatedSha256"] = matrix().virtual_hash(shader, source_patch)
                    cleanup.enter_context(matrix().patched(private, source_patch, output / "reload"))
                    game.send("shaderreload changed")
                terminal = game.wait(lambda: fault_terminal(case, game.evidence, request),
                                     "actual " + case + " owner terminal")
                proof["terminal"] = terminal
                epoch = terminal["payload"].get("recoveryEpoch",
                    terminal["payload"].get("writeEpoch", before["gpu"]["payload"]["submitEpoch"]))
                proof["afterFrame"] = game.wait(lambda: fault_frame_witness(game.evidence.records,
                    snapshot["worldGeneration"], epoch, terminal["sequence"]), "old World later frame/fence")
                proof["after"] = game.wait(lambda: quality_snapshot(game.evidence, output, surfaces,
                    options.surface_id, terminal["sequence"], min_epoch=epoch), "later actual terrain Quality ABI")
                require(proof["after"]["worldGeneration"] == snapshot["worldGeneration"],
                        "Fault replaced the active World")
                if case == "submitFail":
                    proof["completion"] = game.wait(lambda: game.evidence.find("FRAME_ABORT",
                        "old-world-frame-completed", request["faultId"]), "native post-Abort completed frame")
                    completed = proof["completion"][-1]["payload"]
                    require(completed["abortTicket"] == terminal["payload"]["abortTicket"],
                            "Later frame belongs to a different Abort ticket")
                    proof["exactCompletedFrame"] = game.wait(lambda: fault_frame_witness(
                        game.evidence.records, snapshot["worldGeneration"], epoch,
                        terminal["sequence"], completed["submissionEpoch"]), "exact native recovered frame identity")
                    require(proof["exactCompletedFrame"]["gpu"]["payload"]["renderFrame"] == completed["renderFrame"],
                            "Native completion/frame identity differs")
                if case == "staleRevision":
                    actual = terminal["payload"]
                    require(proof["after"]["revision"] >= request["replacementRevision"],
                            "Later actual GPU surface did not bind the newer revision")
                    proof["production"] = next((row for row in raw_production(game.text())
                        if all(row[key] == request[key] for key in ("mip", "x", "y")) and
                        row["slot"] == actual["slot"] and row["reservation"] == actual["reservation"] and
                        row["submissionEpoch"] == actual["writeEpoch"] and
                        row["tokenContentHash"] == actual["oldContentIdentity"]), None)
                    require(proof["production"] is not None, "Stale event lacks exact actual raw task identity")
    proof.update(acceptancePassed=True, status="passed", runtimeEvidence=matrix().identity(output / "runtime-events.jsonl"))
    return proof


def verify_mutated(private, output, options):
    result = matrix().process([options.validator, "--verify-package", private],
                             output, output, "verify-mutated", options.timeout)
    require(matrix().verify_rejection(result) == "native-installation-rejected",
            "Mutated installation did not receive an actual native rejection")
    return {"process": result, "failureLayer": matrix().installation_layer(result)}


def run(options):
    require(options.capture_plan is not None, "Game runner needs --capture-plan")
    require(0 < options.phase_timeout <= 50 and options.timeout > 0, "Invalid bounded wait")
    require(options.status > 0 and options.submit_result < 0 and options.admission_limit > 0,
            "Invalid explicit fault values")
    require(0 <= options.mip <= 31 and 0 <= options.page_x <= 131071 and
            0 <= options.page_y <= 131071 and options.status <= 0xffffffff and
            options.submit_result >= -0x80000000, "Fault page/status/result exceeds native DTO bounds")
    require(0 < options.admission_limit <= 0xffffffffffffffff and
            (options.world_generation is None or 0 < options.world_generation <= 0xffffffffffffffff) and
            (options.revision is None or 0 <= options.revision <= 0xffffffffffffffff) and
            (options.replacement_revision is None or 0 < options.replacement_revision <= 0xffffffffffffffff) and
            (options.slot is None or 0 <= options.slot <= 0xffffffff) and
            (options.slot_publish_epoch is None or 0 < options.slot_publish_epoch <= 0xffffffffffffffff),
            "Explicit identity/admission exceeds native bounds")
    require(options.capture_plan.is_file(), "Capture plan missing")
    shot_names = {row["name"] for row in matrix().read_json(options.capture_plan)["shots"]}
    require({options.baseline_camera, options.camera} <= shot_names, "Actual camera plan names missing")
    packages = [(options.package.resolve(strict=True), None)]
    if options.package_local:
        packages.append((options.package_local.resolve(strict=True), 1))
    selected = list(dict.fromkeys(ALIASES.get(name, name) for name in (options.case or CASES)))
    require(set(selected) <= set(CASES), "Unsupported Game fault case")
    workers = options.workers or [options.worker]
    output, private_root = options.output.resolve(), (options.private_root or options.output / "private").resolve()
    require(not output.exists() and not private_root.exists() and
            not output.is_relative_to(private_root), "Use fresh evidence/private roots")
    jobs, source_before, mutation_plans = [], {}, {}
    for package, mode in packages:
        matrix().regular_tree(package)
        config, manifest, world = matrix().layout(package)
        modes = {row["weightSource"] for row in world["production"]["surfaces"]}
        require(len(modes) == 1 and (mode is None or modes == {mode}), "Package mode differs")
        mode = next(iter(modes))
        require(mode in (0, 1), "Unknown weight mode")
        require(options.surface_id in {row["surfaceId"] for row in world["production"]["surfaces"]},
                "Selected surface does not exist in actual package")
        surface = next((row for row in world.get("surfaces", []) if row["surfaceId"] == options.surface_id), None)
        if surface:
            require(options.page_x < (max(1, surface["width"] >> options.mip) + 127) // 128 and
                    options.page_y < (max(1, surface["height"] >> options.mip) + 127) // 128,
                    "Selected real page is outside the installed layout")
        require(not output.is_relative_to(package) and not package.is_relative_to(output) and
                not private_root.is_relative_to(package) and not package.is_relative_to(private_root),
                "Output/private root overlaps original Game")
        if set(selected) & {"candidate_image", "candidate_raw_kind3", "root_payload",
                            "v1_version", "legacy_material_switch"}:
            recipes = matrix().mutations(package)[1]
            for recipe in recipes:
                recipe["mutatedSha256"] = matrix().virtual_hash(package / recipe["path"], recipe)
            mutation_plans[str(package)] = recipes
        for worker in workers:
            for case in selected:
                label = f"m{mode}-w{worker}-{case}"
                jobs.append({"case": case, "worker": worker, "weightSource": mode,
                             "source": str(package), "privatePackage": str(private_root / label),
                             "evidence": str(output / label), "binary": matrix().identity(package / "main_game.exe")})
        if options.execute:
            require(len(world["production"]["surfaces"]) == 16, "Formal Full16 Game required")
            require(options.binary_sha256 and matrix().sha256(package / "main_game.exe") == options.binary_sha256,
                    "Executed Game must match --binary-sha256 from main's final R9 receipt")
            source_before[str(package)] = matrix().inventory(package)
    require(len({job["evidence"] for job in jobs}) == len(jobs), "Duplicate mode/package jobs")
    for job in jobs:
        command = [sys.executable, str(Path(__file__).with_name("rvt_fault_matrix.py")),
            "--package", job["source"], "--output", job["evidence"],
            "--private-root", job["privatePackage"], "--capture-plan", str(options.capture_plan.resolve()),
            "--worker", str(job["worker"]), "--case", job["case"], "--execute",
            "--binary-sha256", options.binary_sha256 or "<MAIN_FINAL_R9_GAME_SHA256>",
            "--baseline-camera", options.baseline_camera, "--camera", options.camera,
            "--surface-id", options.surface_id, "--mip", str(options.mip),
            "--page-x", str(options.page_x), "--page-y", str(options.page_y),
            "--phase-timeout", str(options.phase_timeout), "--timeout", str(options.timeout),
            "--frames", str(options.frames), "--status", str(options.status),
            "--submit-result", str(options.submit_result), "--admission-limit", str(options.admission_limit),
            "--producer-source", options.producer_source]
        if options.validator:
            command.extend(["--validator", str(options.validator.resolve())])
        for flag, value in (("--world-generation", options.world_generation), ("--revision", options.revision),
                            ("--replacement-revision", options.replacement_revision),
                            ("--slot", options.slot), ("--slot-publish-epoch", options.slot_publish_epoch)):
            if value is not None:
                command.extend([flag, str(value)])
        job["executeCommand"] = command
    report = {"mode": "execute" if options.execute else "plan", "acceptancePassed": False,
              "rendererStarted": False, "jobs": jobs, "requiredWorkers": [1, 2], "requiredWeightSources": [0, 1],
              "mutationPlans": mutation_plans,
              "capturePlan": {"path": str(options.capture_plan.resolve()), **matrix().identity(options.capture_plan)},
              "expectedFinalGameSha256": options.binary_sha256,
              "results": [], "actualIdentityPolicy": "World/revision from actual Quality ABI; no guessed frame shift",
              "slotPolicy": "CLI slot/publish epoch are expectations, only native watch DTO is observation",
              "cacheCases": {"cache_deleted": "N/A disabled optional cache, never pass",
                             "cache_corrupt": "N/A disabled optional cache, never pass"}}
    output.mkdir(parents=True)
    matrix().save(output / "matrix.json", report)
    if not options.execute:
        return 0
    require("slotReuse" not in selected or (options.slot is not None and options.slot_publish_epoch is not None),
            "slotReuse requires explicit expected slot/publish epoch before any Game launch")
    require(options.validator and options.validator.name.lower() == "vl_export_game.exe", "Native verifier required")
    private_root.mkdir(parents=True)
    matrix().save(output / "source-before.json", source_before)
    try:
        for job in jobs:
            private, evidence = Path(job["privatePackage"]), Path(job["evidence"])
            evidence.mkdir()
            (evidence / "reload").mkdir()
            matrix().copy_package(Path(job["source"]), private)
            ownership = {"sourcePackage": job["source"], "privatePackage": str(private.resolve())}
            matrix().save(evidence / "private-copy.json", ownership)
            matrix().save(evidence / "reload/private-copy.json", ownership)
            baseline = matrix().inventory(private)
            matrix().save(evidence / "private-before.json", baseline)
            result = {"case": job["case"], "worker": job["worker"], "weightSource": job["weightSource"],
                      "status": "failed", "acceptancePassed": False}
            try:
                verified = matrix().process([options.validator, "--verify-package", private],
                    evidence, evidence, "verify-before", options.timeout)
                require(verified["exitCode"] == 0 and not verified["timedOut"] and not verified["launchError"],
                        "Baseline native full-package verification failed")
                current_options = SimpleNamespace(**{**vars(options), "worker": job["worker"]})
                result.update(run_case(private, evidence, current_options, job["case"]))
            except (ValueError, OSError, KeyError, TypeError, StopIteration, subprocess.SubprocessError) as error:
                result["error"] = f"{type(error).__name__}: {error}"
            finally:
                result["privateInventoryRestored"] = matrix().inventory(private) == baseline
                if not result["privateInventoryRestored"]:
                    result.update(status="failed", acceptancePassed=False, restoreError="Private installation changed")
                else:
                    restored = matrix().process([options.validator, "--verify-package", private],
                        evidence, evidence, "verify-restored", options.timeout)
                    result["restoredNativeVerify"] = restored
                    if restored["exitCode"] != 0 or restored["timedOut"] or restored["launchError"]:
                        result.update(status="failed", acceptancePassed=False)
                report["rendererStarted"] |= (evidence / "process-start.json").is_file()
                report["results"].append(result)
                matrix().save(evidence / "result.json", result)
                matrix().save(output / "matrix.json", report)
            if not result["acceptancePassed"]:
                break
    finally:
        source_after = {name: matrix().inventory(Path(name)) for name in source_before}
        report["sourceUnchanged"] = source_after == source_before
        matrix().save(output / "source-after.json", source_after)
        report["acceptancePassed"] = len(report["results"]) == len(jobs) and all(
            row["acceptancePassed"] for row in report["results"]) and report["sourceUnchanged"]
        report["status"] = "passed" if report["acceptancePassed"] else "failed"
        report["fullMatrixCovered"] = report["acceptancePassed"] and set(workers) == {1, 2} and \
            {job["weightSource"] for job in jobs} == {0, 1} and set(selected) == set(CASES)
        matrix().save(output / "matrix.json", report)
    return 0 if report["acceptancePassed"] else 2
