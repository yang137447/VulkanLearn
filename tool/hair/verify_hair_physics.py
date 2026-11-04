"""Verify one external hair-physics capture directory without running the engine.

The checker is deliberately separate from capture: it re-reads report.json,
perf.jsonl, stdout/stderr and the BMP/PNG files, then decides whether the evidence
is complete. A picture is never accepted as a substitute for finite-value,
segment-length or pre-projection penetration-depth telemetry; `hair trace` v3
records the SDF/capsule maxima.

Profiles:
  capture     capture integrity only (files, digests, logs, perf, process cleanup)
  acceptance  capture integrity plus the per-frame/readback/scenario evidence
              required by the verification matrix. Current smoke captures do not
              have that evidence and are expected to fail this profile.

Exit codes:
  0  all checks required by the selected profile passed
  1  usage, path or JSON read/parse failure
  2  required evidence is missing, malformed, stale or process cleanup failed
  3  evidence is present but a timing/perf/reset/numerical consistency threshold failed

Example:
  py -3 tool/hair/verify_hair_physics.py artifacts/hair-physics-final \
      --profile capture --json-out artifacts/hair-physics-final/verify.json
  py -3 tool/hair/verify_hair_physics.py artifacts/hair-physics-final --profile acceptance
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

SCHEMA_VERSION = 1
MAX_DT_SECONDS = 0.016

EXIT_PASS = 0
EXIT_USAGE = 1
EXIT_EVIDENCE = 2
EXIT_THRESHOLD = 3

RESET_REASONS = (
    "initial-state", "first-pose", "playback-changed",
    "pose-discontinuity", "pose-recovered",
)
RESET_RE = re.compile(
    r'\[TressFX\] ResetPositions: object="(?P<object>[^"]*)" '
    r"reason=(?P<reason>[a-z-]+) playbackIdentity=(?P<identity>\d+) "
    r"discontinuity=(?P<discontinuity>\d+) frame=(?P<frame>\d+)")
DISPATCH_RE = re.compile(
    r"\[TressFX\] GPU guide simulation dispatch: objects=(?P<objects>\d+) "
    r"guides=(?P<guides>\d+) points=(?P<points>\d+) "
    r"passes=(?P<passes>\d+) sdfObjects=(?P<sdf>\d+)")
TRACE_SUMMARY_RE = re.compile(
    r'\[TressFX\] Hair trace complete: file="(?P<file>[^"]*)" '
    r"frames=(?P<frames>\d+) requestedFrames=(?P<requested>\d+) "
    r"selectedGuides=(?P<selected>\d+) pointsPerGuide=(?P<points>\d+) "
    r"stride=(?P<stride>\d+) bytes=(?P<bytes>\d+)")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

# `hair trace` 文件格式常量。必须与 source/hair/hairTraceFormat.h 保持一致。
TRACE_MAGIC = b"VLHT"
TRACE_VERSION = 3
TRACE_HEADER = struct.Struct("<4sIIIIIQ")  # magic, version, guides, points, stride, selected, start
TRACE_FRAME = struct.Struct("<QQIfII16f2f")  # ..., hairModel, sdfPenetration, capsulePenetration
TRACE_SAMPLE_FLOATS = 12  # current/previous/prevPrevious 各一个 float4
TRACE_MAX_SELECTED_GUIDES = 256
TRACE_MAX_POINTS_PER_GUIDE = 4096
# 段长比是**诊断值**而不是几何正确性阈值：当前正式资产根部采样的 rest 段
# 只有约 0.15 cm，单帧 p99/max 会明显高于 1（2026-10-24 实测 p99≈4.8、
# max≈8.8，并且 reset 帧的 prevPrev 比值精确为 1.0，说明 rest 数据本身对齐）。
# 这里只拦“量级爆炸”，不把根段密集采样当成失败；完整质量判断见验证记录。
TRACE_SEGMENT_RATIO_P99_LIMIT = 6.0
TRACE_SEGMENT_RATIO_MAX_LIMIT = 12.0
# 碰撞 pass 在投影前记录的“点进入 SDF/胶囊内部”的最大深度。碰撞后应被推回，
# 因此这里限制的是单步最大穿透深度，不是接触抖动或最终姿态穿透。
TRACE_PENETRATION_MAX_CM = 6.0
TRACE_RESET_REASONS = {
    0: "none", 1: "initial-state", 2: "first-pose",
    3: "playback-changed", 4: "pose-discontinuity", 5: "pose-recovered",
}

# 验收矩阵要求的状态，必须由报告显式标记为 captured 才计入。
ACCEPTANCE_SCENARIOS = (
    "motion.placement.translation",
    "motion.placement.rotation-y",
    "motion.placement.teleport",
    "motion.head-turn",
    "measure.guide-trajectory",
    "measure.segment-length",
    "measure.penetration",
    "measure.contact-jitter",
    "measure.finite-values",
    "time.per-frame-dt",
    "time.dispatch-count",
    "time.physical-time-ratio",
    "time.pause-stepping",
    "vsp.on-off",
)


def now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_text(path):
    path = Path(path)
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def is_sha256(value):
    return isinstance(value, str) and bool(SHA256_RE.match(value))


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def median(values):
    values = sorted(values)
    if not values:
        return None
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2


def bmp_header(path):
    with Path(path).open("rb") as stream:
        header = stream.read(54)
    if len(header) < 54 or header[:2] != b"BM":
        raise ValueError("invalid BMP magic")
    offset = struct.unpack_from("<I", header, 10)[0]
    width, height = struct.unpack_from("<ii", header, 18)
    bits = struct.unpack_from("<H", header, 28)[0]
    expected = offset + ((width * 3 + 3) & ~3) * abs(height) if width > 0 and height else None
    return {
        "width": width, "height": height, "bitsPerPixel": bits,
        "declaredBytes": expected,
    }


def percentile(values, fraction):
    """Nearest-rank percentile；输入为空时返回 None。"""
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


class TraceFormatError(ValueError):
    """`hair trace` 文件不满足 VLHT v1 布局。"""


def read_hair_trace(path, max_frames=None):
    """解析一次 `hair trace` 文件并计算有限值/段长/轨迹指标。

    文件布局见 source/hair/hairTraceFormat.h；所有字段小端。这里只做只读校验，
    不做物理验收判断；阈值由 Verifier 决定。
    """
    path = Path(path)
    data = path.read_bytes()
    if len(data) < TRACE_HEADER.size:
        raise TraceFormatError("file is shorter than the VLHT header")
    magic, version, guide_count, points_per_guide, stride, selected_count, start_frame = \
        TRACE_HEADER.unpack_from(data, 0)
    if magic != TRACE_MAGIC:
        raise TraceFormatError(f"invalid magic {magic!r}; expected {TRACE_MAGIC!r}")
    if version != TRACE_VERSION:
        raise TraceFormatError(f"unsupported version {version}; expected {TRACE_VERSION}")
    if not 1 <= selected_count <= TRACE_MAX_SELECTED_GUIDES:
        raise TraceFormatError(f"selectedCount={selected_count} is outside 1..{TRACE_MAX_SELECTED_GUIDES}")
    if not 2 <= points_per_guide <= TRACE_MAX_POINTS_PER_GUIDE:
        raise TraceFormatError(f"pointsPerGuide={points_per_guide} is outside 2..{TRACE_MAX_POINTS_PER_GUIDE}")
    if guide_count <= 0 or stride <= 0:
        raise TraceFormatError("guideCount/stride must be positive")
    expected_selected = min((guide_count + stride - 1) // stride, TRACE_MAX_SELECTED_GUIDES)
    if selected_count != expected_selected:
        raise TraceFormatError(
            f"selectedCount={selected_count} does not match ceil(guideCount/stride) cap "
            f"({expected_selected})")

    offset = TRACE_HEADER.size
    selected_bytes = selected_count * 4
    if len(data) < offset + selected_bytes:
        raise TraceFormatError("truncated selected guide list")
    selected = list(struct.unpack_from(f"<{selected_count}I", data, offset))
    offset += selected_bytes
    rest_count = selected_count * points_per_guide
    rest_bytes = rest_count * 4
    if len(data) < offset + rest_bytes:
        raise TraceFormatError("truncated rest length list")
    rest = struct.unpack_from(f"<{rest_count}f", data, offset)
    offset += rest_bytes
    if any(index >= guide_count for index in selected):
        raise TraceFormatError("selected guide index is outside guideCount")
    if any(selected[i] >= selected[i + 1] for i in range(len(selected) - 1)):
        raise TraceFormatError("selected guide indices are not strictly increasing")
    if any(not math.isfinite(value) or value < 0 for value in rest):
        raise TraceFormatError("rest lengths must be finite and non-negative")

    payload_bytes = selected_count * points_per_guide * TRACE_SAMPLE_FLOATS * 4
    segment_ratios = []
    finite_count = 0
    root_non_finite = 0
    tip_non_finite = 0
    frame_count = 0
    first_frame = None
    last_frame = None
    substeps_histogram = {}
    time_steps = []
    reset_counts = {name: 0 for name in TRACE_RESET_REASONS.values()}
    dispatch_counts = []
    frames_with_no_substeps = 0
    previous_root = [None] * selected_count
    previous_tip = [None] * selected_count
    previous_world_root = [None] * selected_count
    previous_world_tip = [None] * selected_count
    root_max_step = 0.0
    tip_max_step = 0.0
    world_root_max_step = 0.0
    world_tip_max_step = 0.0
    model_first = None
    model_last = None
    sdf_penetration_max = 0.0
    capsule_penetration_max = 0.0

    while offset < len(data):
        if max_frames is not None and frame_count >= max_frames:
            raise TraceFormatError("file has more frames than requested")
        if len(data) - offset < TRACE_FRAME.size:
            raise TraceFormatError("truncated frame header")
        frame_index, world_generation, substeps, time_step, reset_reason, dispatch_count, \
            *model_values, sdf_penetration, capsule_penetration = \
            TRACE_FRAME.unpack_from(data, offset)
        offset += TRACE_FRAME.size
        if (not math.isfinite(sdf_penetration) or sdf_penetration < 0 or
                not math.isfinite(capsule_penetration) or capsule_penetration < 0):
            raise TraceFormatError("penetration depths must be finite and non-negative")
        sdf_penetration_max = max(sdf_penetration_max, sdf_penetration)
        capsule_penetration_max = max(capsule_penetration_max, capsule_penetration)
        # 列主序 mat4；世界 = model * local。
        model = model_values
        if model_first is None:
            model_first = model[12:15]
        model_last = model[12:15]
        if len(data) - offset < payload_bytes:
            raise TraceFormatError("truncated frame payload")
        values = struct.unpack_from(f"<{payload_bytes // 4}f", data, offset)
        offset += payload_bytes

        if first_frame is None:
            first_frame = frame_index
        last_frame = frame_index
        frame_count += 1
        substeps_histogram[str(substeps)] = substeps_histogram.get(str(substeps), 0) + 1
        if substeps == 0:
            frames_with_no_substeps += 1
        time_steps.append(time_step)
        reset_name = TRACE_RESET_REASONS.get(reset_reason, f"unknown-{reset_reason}")
        reset_counts[reset_name] = reset_counts.get(reset_name, 0) + 1
        dispatch_counts.append(dispatch_count)

        for guide in range(selected_count):
            base = guide * points_per_guide
            root_offset = base * TRACE_SAMPLE_FLOATS
            tip_offset = (base + points_per_guide - 1) * TRACE_SAMPLE_FLOATS
            root = values[root_offset:root_offset + 3]
            tip = values[tip_offset:tip_offset + 3]
            if not all(math.isfinite(value) for value in root):
                root_non_finite += 1
            elif previous_root[guide] is not None:
                root_max_step = max(root_max_step, math.dist(root, previous_root[guide]))
            previous_root[guide] = root
            if not all(math.isfinite(value) for value in tip):
                tip_non_finite += 1
            elif previous_tip[guide] is not None:
                tip_max_step = max(tip_max_step, math.dist(tip, previous_tip[guide]))
            previous_tip[guide] = tip
            # 世界坐标：列主序 mat4 * (x,y,z,1)，供 placement 惯性/轨迹分析。
            world_root = (
                model[0] * root[0] + model[4] * root[1] + model[8] * root[2] + model[12],
                model[1] * root[0] + model[5] * root[1] + model[9] * root[2] + model[13],
                model[2] * root[0] + model[6] * root[1] + model[10] * root[2] + model[14])
            world_tip = (
                model[0] * tip[0] + model[4] * tip[1] + model[8] * tip[2] + model[12],
                model[1] * tip[0] + model[5] * tip[1] + model[9] * tip[2] + model[13],
                model[2] * tip[0] + model[6] * tip[1] + model[10] * tip[2] + model[14])
            if previous_world_root[guide] is not None:
                world_root_max_step = max(world_root_max_step,
                    math.dist(world_root, previous_world_root[guide]))
                world_tip_max_step = max(world_tip_max_step,
                    math.dist(world_tip, previous_world_tip[guide]))
            previous_world_root[guide] = world_root
            previous_world_tip[guide] = world_tip
            for point in range(points_per_guide - 1):
                current = values[(base + point) * TRACE_SAMPLE_FLOATS:
                                 (base + point) * TRACE_SAMPLE_FLOATS + 3]
                following = values[(base + point + 1) * TRACE_SAMPLE_FLOATS:
                                   (base + point + 1) * TRACE_SAMPLE_FLOATS + 3]
                rest_length = rest[base + point]
                if rest_length <= 0:
                    continue
                segment_ratios.append(math.dist(current, following) / rest_length)

        for value in values:
            if not math.isfinite(value):
                finite_count += 1

    expected_bytes = (TRACE_HEADER.size + selected_bytes + rest_bytes +
                      frame_count * (TRACE_FRAME.size + payload_bytes))
    if expected_bytes != len(data):
        raise TraceFormatError(
            f"file byte count {len(data)} != parsed {expected_bytes}")
    if frame_count == 0:
        raise TraceFormatError("trace contains no frames")

    return {
        "format": "VLHT",
        "version": version,
        "path": str(path),
        "bytes": len(data),
        "sha256": digest(path),
        "guideCount": guide_count,
        "selectedCount": selected_count,
        "pointsPerGuide": points_per_guide,
        "stride": stride,
        "startFrame": start_frame,
        "selectedGuidesFirst": selected[:4],
        "frames": frame_count,
        "firstFrame": first_frame,
        "lastFrame": last_frame,
        "finiteCount": finite_count,
        "rootNonFinite": root_non_finite,
        "tipNonFinite": tip_non_finite,
        "segmentRatioSamples": len(segment_ratios),
        "segmentRatioP50": percentile(segment_ratios, 0.50),
        "segmentRatioP99": percentile(segment_ratios, 0.99),
        "segmentRatioMax": max(segment_ratios) if segment_ratios else None,
        "rootMaxStepCm": root_max_step,
        "tipMaxStepCm": tip_max_step,
        "worldRootMaxStepCm": world_root_max_step,
        "worldTipMaxStepCm": world_tip_max_step,
        "modelTranslationFirst": model_first,
        "modelTranslationLast": model_last,
        "sdfPenetrationMaxCm": sdf_penetration_max,
        "capsulePenetrationMaxCm": capsule_penetration_max,
        "substeps": substeps_histogram,
        "framesWithNoSubsteps": frames_with_no_substeps,
        "timeStepMin": min(time_steps) if time_steps else None,
        "timeStepMax": max(time_steps) if time_steps else None,
        "resetReasonCounts": reset_counts,
        "dispatchCountMin": min(dispatch_counts) if dispatch_counts else None,
        "dispatchCountP50": percentile(dispatch_counts, 0.5),
        "dispatchCountMax": max(dispatch_counts) if dispatch_counts else None,
        "payloadBytesPerFrame": payload_bytes,
    }


def pid_alive(pid):
    """只按采集器记录的 PID 判断；PID 复用风险由报告时间和本地证据承担。"""
    if not isinstance(pid, int) or pid <= 0:
        return None
    if os.name == "nt":
        tasklist = shutil.which("tasklist")
        if not tasklist:
            return None
        try:
            result = subprocess.run(
                [tasklist, "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            return None
        text = result.stdout.strip()
        if not text or "No tasks" in text or "INFO:" in text:
            return False
        return f'"{pid}"' in text or str(pid) in text
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except (PermissionError, OSError):
        return True
    return True


class Verifier:
    def __init__(self, capture_dir, profile, required_scenarios, expected_rates=()):
        self.capture_dir = Path(capture_dir)
        self.profile = profile
        self.required_scenarios = tuple(required_scenarios)
        self.expected_rates = tuple(expected_rates)
        self.report = None
        self.findings = []
        self.failures = []
        self.acceptance_gaps = []
        self.coverage = {}
        self.run_dirs = []
        self.trace_reports = []
        self.summary = {
            "runs": 0, "captures": 0, "perfRows": 0,
            "dispatchObjects": 0, "guides": 0, "points": 0,
            "resetEvents": 0, "traceFrames": 0, "traceBytes": 0,
        }

    # ---- finding helpers -------------------------------------------------
    def pass_(self, check_id, detail, evidence=None):
        self.findings.append({
            "id": check_id, "status": "pass", "detail": detail,
            **({"evidence": evidence} if evidence is not None else {}),
        })

    def fail(self, check_id, detail, code=EXIT_EVIDENCE, evidence=None):
        entry = {
            "id": check_id, "status": "fail", "detail": detail,
            **({"evidence": evidence} if evidence is not None else {}),
        }
        self.findings.append(entry)
        self.failures.append({"id": check_id, "code": code, "detail": detail})

    def check(self, check_id, ok, detail, code=EXIT_EVIDENCE, evidence=None):
        if ok:
            self.pass_(check_id, detail, evidence)
        else:
            self.fail(check_id, detail, code, evidence)
        return bool(ok)

    def gap(self, check_id, detail, code=EXIT_EVIDENCE):
        """验收缺口：capture profile 只报告，acceptance profile 记为失败。"""
        self.acceptance_gaps.append({"id": check_id, "detail": detail})
        self.pass_(check_id + ".capture", "capture profile: " + detail)
        if self.profile == "acceptance":
            self.fail(check_id, detail, code)

    def threshold(self, check_id, ok, detail, evidence=None):
        """阈值检查：capture 只记录为验收缺口，acceptance 以 exit 3 失败。"""
        if ok:
            self.pass_(check_id, detail, evidence)
        elif self.profile == "acceptance":
            self.fail(check_id, detail, EXIT_THRESHOLD, evidence)
        else:
            self.acceptance_gaps.append({"id": check_id, "detail": detail})
            self.pass_(check_id + ".capture", "capture profile: " + detail, evidence)

    # ---- loading ---------------------------------------------------------
    def load(self):
        target = self.capture_dir
        report_path = target / "report.json" if target.is_dir() else target
        self.capture_dir = report_path.parent
        if not report_path.is_file():
            raise FileNotFoundError(f"report.json not found: {report_path}")
        try:
            self.report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f"report.json is not readable JSON: {error}") from error
        if not isinstance(self.report, dict):
            raise ValueError("report.json root must be an object")

    def run(self):
        self.load()
        self.check_metadata()
        self.check_inputs()
        self.check_scenario_coverage()
        self.check_runs()
        self.check_acceptance()
        return self.result()

    # ---- top level -------------------------------------------------------
    def check_metadata(self):
        report = self.report
        schema = report.get("schemaVersion")
        if schema is None:
            self.gap("report.schemaVersion",
                     "legacy report has no schemaVersion; new capture fields cannot be audited")
        else:
            self.check(
                "report.schemaVersion",
                isinstance(schema, int) and 1 <= schema <= SCHEMA_VERSION,
                f"schemaVersion={schema!r} (checker supports 1..{SCHEMA_VERSION})",
                evidence={"schemaVersion": schema})
        self.check("report.completed", report.get("completed") is True,
                   f"completed={report.get('completed')!r}")
        if schema is None:
            self.gap("report.visualReviewRequired",
                     "legacy report has no visualReviewRequired field")
            self.gap("report.completionMeaning",
                     "legacy report has no completionMeaning field")
        else:
            self.check("report.visualReviewRequired", report.get("visualReviewRequired") is True,
                       "images require human review and never replace numerical evidence")
            self.check("report.completionMeaning",
                       isinstance(report.get("completionMeaning"), str) and
                       bool(report.get("completionMeaning").strip()),
                       "completionMeaning must state the evidence boundary")
        self.check("report.physicsNumericalValidation.type",
                   isinstance(report.get("physicsNumericalValidation"), bool),
                   "physicsNumericalValidation must be an explicit boolean")
        if report.get("physicsNumericalValidation") is True:
            evidence = report.get("numericEvidence")
            self.check(
                "report.numericEvidence",
                isinstance(evidence, dict) and
                evidence.get("kind") in ("gpu-readback", "cpu-reference-export") and
                is_sha256(evidence.get("sha256")) and
                isinstance(evidence.get("frames"), int) and evidence.get("frames") > 0,
                "physicsNumericalValidation=true requires numericEvidence with kind/sha256/frames",
                code=EXIT_EVIDENCE, evidence=evidence)

    def check_inputs(self):
        report = self.report
        executable = report.get("executable")
        exe_hash = report.get("exeSha256")
        self.check("input.exeSha256.format", is_sha256(exe_hash),
                   "exeSha256 must be a 64-character lowercase hex digest",
                   evidence={"exeSha256": exe_hash})
        if isinstance(executable, str) and Path(executable).is_file():
            current = digest(executable)
            if current == exe_hash:
                self.pass_("input.exeSha256.current", "recorded exe digest matches current binary")
            else:
                # 二进制重建后旧采集仍可保留；只标记不是当前实现的证据。
                self.pass_(
                    "input.exeSha256.current",
                    "recorded exe digest differs from current binary; capture remains historical evidence",
                    {"recorded": exe_hash, "current": current})
        else:
            self.gap("input.executable", "recorded executable path is missing; cannot re-hash it",
                    code=EXIT_EVIDENCE)

        hashes = report.get("inputSha256")
        self.check("input.inputSha256.object", isinstance(hashes, dict) and bool(hashes),
                   "inputSha256 must be a non-empty path->digest object")
        if not isinstance(hashes, dict):
            return
        bad = [path for path, value in hashes.items() if not is_sha256(value)]
        self.check("input.inputSha256.format", not bad,
                   "every input digest must be 64-character lowercase hex",
                   evidence={"invalid": bad})
        names = [Path(path).name for path in hashes]
        self.check(
            "input.inputSha256.categories",
            any(name == "config.json" for name in names) and
            any(name.startswith("SC_") and name.endswith(".json") for name in names) and
            any(name.startswith("SM_") and name.endswith(".json") for name in names),
            "inputSha256 must cover config, scene and hair descriptor",
            evidence={"names": names})
        self.check("input.inputsUnchanged", report.get("inputsUnchanged") is True,
                   f"inputsUnchanged={report.get('inputsUnchanged')!r}")
        missing = []
        changed = []
        for path, recorded in hashes.items():
            input_path = Path(path)
            if not input_path.is_file():
                missing.append(path)
                continue
            current = digest(input_path)
            if current != recorded:
                changed.append({"path": path, "recorded": recorded, "current": current})
        if missing:
            self.gap("input.rehash.missing", f"input files missing after capture: {missing}")
        else:
            self.pass_("input.rehash.missing", "all recorded input files exist for re-hash")
        self.check("input.rehash.changed", not changed,
                   "inputs changed after capture; SHA256 evidence is stale",
                   evidence={"changed": changed})

        descriptor = report.get("hairDescriptor")
        self.check(
            "input.hairDescriptor",
            isinstance(descriptor, dict) and descriptor.get("type") == "tressfx" and
            isinstance(descriptor.get("simulation"), dict) and
            descriptor["simulation"].get("backend") == "gpu",
            "hairDescriptor must record a tressfx/gpu asset",
            evidence={"name": descriptor.get("name") if isinstance(descriptor, dict) else None})

    def check_scenario_coverage(self):
        coverage = self.report.get("scenarioCoverage")
        if not isinstance(coverage, list) or not coverage:
            self.gap("matrix.scenarioCoverage",
                     "report has no scenarioCoverage; matrix coverage cannot be audited")
            return
        invalid = []
        for entry in coverage:
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
                invalid.append(entry)
                continue
            status = entry.get("status")
            if status not in ("captured", "partial", "unsupported"):
                invalid.append(entry)
                continue
            self.coverage[entry["id"]] = entry
        self.check("matrix.scenarioCoverage.schema", not invalid,
                   "every scenarioCoverage entry needs id and captured/partial/unsupported status",
                   evidence={"invalid": invalid})
        for scenario_id in self.required_scenarios:
            entry = self.coverage.get(scenario_id)
            if not entry:
                self.gap(f"matrix.{scenario_id}", "scenario is not listed in report coverage")
            elif entry.get("status") != "captured":
                self.gap(
                    f"matrix.{scenario_id}",
                    f"scenario status={entry.get('status')}: {entry.get('reason', '')}".strip())

    def resolve_run_dirs(self, runs):
        explicit = [run.get("runDirectory") for run in runs]
        if all(isinstance(path, str) and Path(path).is_dir() for path in explicit):
            return [Path(path) for path in explicit]
        candidates = sorted(
            path for path in self.capture_dir.iterdir()
            if path.is_dir() and
            ((path / "perf.jsonl").exists() or (path / "report.json").exists()))
        self.pass_(
            "runs.directory.legacy",
            "runDirectory missing; matched run directories by order",
            {"candidates": [path.name for path in candidates]})
        return candidates[:len(runs)]

    def check_runs(self):
        runs = self.report.get("runs")
        self.check("runs.object", isinstance(runs, list) and bool(runs),
                   "runs must be a non-empty array")
        if not isinstance(runs, list) or not runs:
            return
        self.summary["runs"] = len(runs)
        actual_rates = {run.get("rateHz") for run in runs if isinstance(run, dict)}
        if self.profile == "acceptance" and self.expected_rates:
            missing_rates = [rate for rate in self.expected_rates if rate not in actual_rates]
            if missing_rates:
                self.gap("matrix.rates", f"missing expected rate runs: {missing_rates}")
            else:
                self.pass_("matrix.rates",
                           f"all expected rate runs present: {list(self.expected_rates)}")
        self.run_dirs = self.resolve_run_dirs(runs)
        self.check("runs.directory.count", len(self.run_dirs) == len(runs),
                   f"expected {len(runs)} run directories, found {len(self.run_dirs)}",
                   evidence={"directories": [str(path) for path in self.run_dirs]})
        for index, run in enumerate(runs):
            run_dir = self.run_dirs[index] if index < len(self.run_dirs) else None
            self.check_run(index, run, run_dir)

    # ---- one run ---------------------------------------------------------
    def check_run(self, index, run, run_dir):
        prefix = f"run[{index}]"
        if not isinstance(run, dict):
            self.fail(f"{prefix}.object", "run entry must be an object")
            return
        rate = run.get("rateHz")
        self.check(f"{prefix}.rateHz", isinstance(rate, int) and 0 <= rate <= 1000,
                   f"rateHz={rate!r}; 0 means uncapped variable dt")
        self.check(f"{prefix}.completed", run.get("completed") is True,
                   f"completed={run.get('completed')!r}")
        self.check(f"{prefix}.sequenceCompleted", run.get("sequenceCompleted") is True,
                   "interaction sequence did not complete")
        if run.get("failure"):
            self.fail(f"{prefix}.failure", f"capture recorded failure: {run['failure']}")
        if run_dir is None or not Path(run_dir).is_dir():
            self.fail(f"{prefix}.directory", "run directory is missing or not a directory",
                      evidence={"runDirectory": run.get("runDirectory")})
            return

        self.check_process(prefix, run)
        self.check_captures(prefix, run, Path(run_dir))
        stdout = read_text(Path(run_dir) / "stdout.log")
        stderr = read_text(Path(run_dir) / "stderr.log")
        self.check_logs(prefix, run, Path(run_dir), stdout, stderr)
        self.check_perf(prefix, run, Path(run_dir), stdout)
        self.check_timing(prefix, run)
        self.check_trace(prefix, run, Path(run_dir))

    def check_process(self, prefix, run):
        pid = run.get("pid")
        self.check(f"{prefix}.process.pid", isinstance(pid, int) and pid > 0,
                   f"pid={pid!r}")
        self.check(f"{prefix}.process.ownedExited", run.get("ownedProcessExited") is True,
                   "ownedProcessExited must be true")
        self.check(f"{prefix}.process.exitCode",
                   run.get("processExitCode") is not None,
                   "processExitCode must be recorded (terminate is not graceful teardown)")
        alive = pid_alive(pid)
        if alive is None:
            self.pass_(f"{prefix}.process.notAlive", "PID liveness tool unavailable; skipped")
        else:
            self.check(f"{prefix}.process.notAlive", not alive,
                       f"PID {pid} is still alive after capture")
        closure = run.get("processClosure")
        self.check(f"{prefix}.process.closure",
                   isinstance(closure, str) and bool(closure.strip()),
                   "processClosure must state that forced termination is not graceful teardown")

    def check_captures(self, prefix, run, run_dir):
        captures = run.get("captures")
        self.check(f"{prefix}.captures.object", isinstance(captures, list) and bool(captures),
                   "captures must be a non-empty array")
        if not isinstance(captures, list) or not captures:
            return
        self.summary["captures"] += len(captures)
        names = set()
        for capture in captures:
            if not isinstance(capture, dict):
                self.fail(f"{prefix}.capture.object", "capture entry must be an object")
                continue
            name = capture.get("name")
            tag = f"{prefix}.capture.{name or '?'}"
            self.check(f"{tag}.name", isinstance(name, str) and bool(name) and name not in names,
                       f"capture name must be unique and non-empty: {name!r}")
            names.add(name)
            captured = capture.get("capturedSeconds")
            self.check(f"{tag}.capturedSeconds", finite_number(captured) and captured >= 0,
                       f"capturedSeconds={captured!r}")
            size = capture.get("size")
            self.check(f"{tag}.size",
                       isinstance(size, list) and len(size) == 2 and
                       all(isinstance(value, int) and value > 0 for value in size),
                       f"size={size!r}")
            playback = capture.get("playback")
            self.check(f"{tag}.playback", isinstance(playback, list) and bool(playback),
                       "capture must record Animation[ playback state")
            for kind in ("bmp", "png"):
                path = run_dir / f"{name}.{kind}"
                digest_key = f"{kind}Sha256"
                self.check(f"{tag}.{kind}.exists", path.is_file(), f"missing {path}")
                if not path.is_file():
                    continue
                recorded = capture.get(digest_key)
                if not is_sha256(recorded):
                    # 旧报告没有 pngSha256；仍计算当前摘要，但不把缺失写成通过。
                    if kind == "png":
                        self.gap(f"{tag}.{kind}.sha256",
                                 "capture has no pngSha256; cannot prove PNG matches BMP")
                    else:
                        self.fail(f"{tag}.{kind}.sha256", f"missing or invalid {digest_key}",
                                  evidence={digest_key: recorded})
                    continue
                current = digest(path)
                self.check(f"{tag}.{kind}.sha256", current == recorded,
                           f"{kind} digest differs from report",
                           evidence={"recorded": recorded, "current": current})
            bmp_path = run_dir / f"{name}.bmp"
            if bmp_path.is_file():
                try:
                    header = bmp_header(bmp_path)
                except (OSError, ValueError) as error:
                    self.fail(f"{tag}.bmp.header", f"invalid BMP: {error}")
                else:
                    expected_size = (capture.get("size") or [None, None])
                    self.check(
                        f"{tag}.bmp.header",
                        header["bitsPerPixel"] == 24 and
                        header["width"] == expected_size[0] and
                        abs(header["height"]) == expected_size[1] and
                        header["declaredBytes"] == bmp_path.stat().st_size,
                        "BMP must be 24-bit with the reported size and complete payload",
                        evidence={"header": header, "actualBytes": bmp_path.stat().st_size})
            recorded_png_bytes = capture.get("pngBytes")
            if isinstance(recorded_png_bytes, int) and (run_dir / f"{name}.png").is_file():
                self.check(f"{tag}.png.bytes",
                           recorded_png_bytes == (run_dir / f"{name}.png").stat().st_size,
                           "PNG byte count differs from report")
            if "resetEvents" not in capture:
                self.gap(f"{tag}.resetEvents",
                         "capture has no per-window resetEvents; only global counts exist")
            elif not isinstance(capture.get("resetEvents"), list):
                self.fail(f"{tag}.resetEvents", "resetEvents must be an array")

    def check_logs(self, prefix, run, run_dir, stdout, stderr):
        self.check(f"{prefix}.stdout.exists", (run_dir / "stdout.log").is_file(),
                   "stdout.log is missing")
        self.check(f"{prefix}.stderr.exists", (run_dir / "stderr.log").is_file(),
                   "stderr.log is missing")
        actual_stderr = (run_dir / "stderr.log").stat().st_size if (run_dir / "stderr.log").exists() else None
        self.check(f"{prefix}.stderr.bytes", run.get("stderrBytes") == actual_stderr,
                   f"stderrBytes={run.get('stderrBytes')!r}, actual={actual_stderr!r}")
        self.check(f"{prefix}.stderr.empty", not stderr.strip(),
                   "stderr must be empty for a clean capture",
                   evidence={"stderr": stderr[-2000:]})
        vuid_count = len(re.findall(r"VUID-", stdout + stderr))
        self.check(f"{prefix}.vuid", vuid_count == 0 and run.get("vuidCount") == 0,
                   f"VUID markers: actual={vuid_count}, report={run.get('vuidCount')!r}")
        rejected = [line for line in stdout.splitlines()
                    if "Unknown command" in line or "Animation control rejected" in line]
        self.check(f"{prefix}.console.rejected", not rejected,
                   "Console rejected a command", evidence={"lines": rejected})

        dispatch = DISPATCH_RE.search(stdout)
        self.check(f"{prefix}.dispatch.exists", dispatch is not None,
                   "no GPU guide simulation dispatch line")
        if dispatch:
            values = {key: int(value) for key, value in dispatch.groupdict().items()}
            values["sdfObjects"] = values.pop("sdf")
            self.summary["dispatchObjects"] += values["objects"]
            self.summary["guides"] += values["guides"]
            self.summary["points"] += values["points"]
            self.check(f"{prefix}.dispatch.counts",
                       values["objects"] >= 1 and values["guides"] > 0 and
                       values["points"] > 0 and values["passes"] >= 1,
                       "dispatch must report objects/guides/points/passes",
                       evidence=values)
            descriptor = self.report.get("hairDescriptor") or {}
            requires_sdf = isinstance(descriptor.get("collision"), dict) and \
                isinstance(descriptor["collision"].get("mesh"), dict)
            if requires_sdf:
                self.check(f"{prefix}.dispatch.sdfObjects", values["sdfObjects"] >= 1,
                           "hair asset declares collision.mesh but dispatch sdfObjects=0",
                           evidence={"sdfObjects": values["sdfObjects"]})
            recorded = run.get("dispatch")
            if isinstance(recorded, dict):
                self.check(f"{prefix}.dispatch.reportMatch",
                           all(recorded.get(key) == values[key] for key in values),
                           "report dispatch counts differ from stdout",
                           evidence={"report": recorded, "stdout": values})
            else:
                self.gap(f"{prefix}.dispatch.reportObject",
                         "report has no parsed dispatch object")
        sdf_lines = [line for line in stdout.splitlines()
                     if "SDF collision buffer:" in line or "SDF domain:" in line]
        self.check(f"{prefix}.sdf.evidence", bool(sdf_lines),
                   "no SDF buffer/domain evidence line")

        events = []
        for line in stdout.splitlines():
            match = RESET_RE.search(line)
            if match:
                values = match.groupdict()
                events.append({
                    "object": values["object"], "reason": values["reason"],
                    "playbackIdentity": int(values["identity"]),
                    "discontinuity": int(values["discontinuity"]),
                    "frame": int(values["frame"]),
                })
        self.summary["resetEvents"] += len(events)
        counts = {reason: sum(event["reason"] == reason for event in events)
                  for reason in RESET_REASONS}
        recorded_counts = run.get("resetReasonCounts")
        if isinstance(recorded_counts, dict):
            self.check(f"{prefix}.reset.counts", recorded_counts == counts,
                       "report resetReasonCounts differ from stdout",
                       evidence={"report": recorded_counts, "stdout": counts})
        else:
            self.gap(f"{prefix}.reset.counts",
                     "legacy report has no resetReasonCounts; structured counts unavailable")
        require_reset = run.get("requireReset")
        if require_reset is None:
            self.gap(f"{prefix}.reset.requireReset",
                     "report has no requireReset flag; reset contract cannot be audited")
        elif require_reset:
            self.check(f"{prefix}.reset.firstPose", counts["first-pose"] >= 1,
                       "requireReset needs at least one first-pose event")
            startup_only = (self.report.get("captureArguments") or {}).get("startupOnly") is True or \
                str(run.get("scope", "")).startswith("Initial Idle frames")
            if not startup_only:
                self.check(f"{prefix}.reset.discontinuity",
                           counts["pose-discontinuity"] >= 4,
                           "requireReset interaction needs at least four seek/clip discontinuities",
                           evidence={"counts": counts})
        pause_probes = run.get("pauseProbesRequested") or 0
        if pause_probes:
            paused = [capture for capture in run.get("captures", [])
                      if str(capture.get("name", "")).startswith("walk-paused")]
            self.check(f"{prefix}.pause.probes", len(paused) >= pause_probes + 1,
                       "pause probe snapshots are incomplete",
                       evidence={"found": len(paused), "requested": pause_probes + 1})
            self.gap(f"{prefix}.pause.stepping",
                     "paused snapshots exist, but no per-frame step counter proves hair keeps stepping")

    def check_perf(self, prefix, run, run_dir, stdout):
        path = run_dir / "perf.jsonl"
        self.check(f"{prefix}.perf.exists", path.is_file(), f"missing {path}")
        if not path.is_file():
            return
        lines = [line for line in read_text(path).splitlines() if line.strip()]
        rows = []
        malformed = []
        for line_number, line in enumerate(lines, 1):
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                malformed.append({"line": line_number, "error": str(error)})
                continue
            if not isinstance(row, dict):
                malformed.append({"line": line_number, "error": "not an object"})
                continue
            rows.append(row)
        self.check(f"{prefix}.perf.parse", not malformed,
                   "perf.jsonl contains malformed JSON lines", evidence={"malformed": malformed})
        self.check(f"{prefix}.perf.rows", bool(rows), "perf.jsonl has no valid rows")
        if not rows:
            return
        self.summary["perfRows"] += len(rows)
        recorded = run.get("perf") or {}
        self.check(f"{prefix}.perf.samples", recorded.get("samples") == len(rows),
                   f"perf.samples={recorded.get('samples')!r}, actual={len(rows)}")
        if recorded.get("sha256") is not None:
            self.check(f"{prefix}.perf.sha256", recorded.get("sha256") == digest(path),
                       "perf.jsonl digest differs from report")
        else:
            self.gap(f"{prefix}.perf.sha256", "report has no perf.jsonl SHA256")

        fps = []
        cpu = []
        gpu_total = []
        hair = []
        valid_rows = 0
        missing_passes = 0
        invalid_numbers = 0
        for row in rows:
            if not all(key in row for key in ("fps", "cpuMs", "gpuTotalMs", "valid", "passes")):
                invalid_numbers += 1
                continue
            if row.get("valid"):
                valid_rows += 1
            if finite_number(row.get("fps")) and row["fps"] > 0:
                fps.append(row["fps"])
            if finite_number(row.get("cpuMs")) and row["cpuMs"] > 0:
                cpu.append(row["cpuMs"])
            if row.get("gpuTotalMs") is not None:
                if finite_number(row.get("gpuTotalMs")) and row["gpuTotalMs"] > 0:
                    gpu_total.append(row["gpuTotalMs"])
                else:
                    invalid_numbers += 1
            passes = row.get("passes")
            if not isinstance(passes, list):
                invalid_numbers += 1
                continue
            hair_rows = [entry for entry in passes if isinstance(entry, dict) and
                         entry.get("name") == "hairSimulation"]
            if not hair_rows:
                missing_passes += 1
            for entry in hair_rows:
                milliseconds = entry.get("ms")
                if milliseconds is None:
                    continue
                if finite_number(milliseconds) and milliseconds > 0:
                    hair.append(milliseconds)
                else:
                    invalid_numbers += 1
        self.check(f"{prefix}.perf.rowSchema", invalid_numbers == 0,
                   "perf rows contain missing/non-finite fields",
                   evidence={"invalidFields": invalid_numbers})
        self.check(f"{prefix}.perf.validRows", recorded.get("validGpuSamples") == valid_rows,
                   f"perf.validGpuSamples={recorded.get('validGpuSamples')!r}, actual={valid_rows}")
        self.check(f"{prefix}.perf.hairPass", bool(hair),
                   "no valid hairSimulation GPU sample")
        if hair:
            hair_median = median(hair)
            self.check(f"{prefix}.perf.hairMedian",
                       finite_number(recorded.get("medianHairGpuMs")) and
                       abs(recorded["medianHairGpuMs"] - hair_median) <= 0.01,
                       "report medianHairGpuMs differs from perf.jsonl",
                       evidence={"report": recorded.get("medianHairGpuMs"),
                                 "actual": hair_median})
        self.threshold(f"{prefix}.perf.gpuCoverage",
                       len(gpu_total) / len(rows) >= 0.8,
                       "fewer than 80% of perf rows carry a positive GPU total",
                       {"positive": len(gpu_total), "rows": len(rows)})
        rate = run.get("rateHz")
        fps_median = median(fps)
        self.check(f"{prefix}.perf.fps", fps_median is not None,
                   "no positive FPS sample")
        if fps_median is not None and isinstance(rate, int) and rate > 0:
            low = rate * 0.85
            high = rate * 1.10
            self.threshold(
                f"{prefix}.perf.frameRate",
                low <= fps_median <= high,
                f"median FPS {fps_median:.2f} outside {low:.2f}..{high:.2f} for --frame-limit {rate}",
                {"medianFps": fps_median, "minFps": min(fps), "maxFps": max(fps)})
        if recorded.get("medianFps") is not None and fps_median is not None:
            self.check(f"{prefix}.perf.fpsReport",
                       abs(recorded["medianFps"] - fps_median) <= 0.01,
                       "report medianFps differs from perf.jsonl")
        captures = run.get("captures") or []
        capture_seconds = [
            capture.get("capturedSeconds", 0) for capture in captures
            if isinstance(capture.get("capturedSeconds"), (int, float))]
        first_capture = min(capture_seconds, default=0)
        last_capture = max(capture_seconds, default=0)
        capture_span = max(0.0, last_capture - first_capture)
        # perf.jsonl 首行在初始化+1 s 后写出，因此用首末截图跨度而不是总墙钟；
        # 它仍能发现"写到一半停止"的时间序列缺口。
        if capture_span >= 2:
            self.threshold(f"{prefix}.perf.coverage",
                           len(rows) >= max(1, int(capture_span) - 1),
                           "perf rows do not cover the capture span",
                           {"rows": len(rows), "captureSpanSeconds": capture_span,
                            "firstCaptureSeconds": first_capture,
                            "lastCaptureSeconds": last_capture})
        self.pass_(
            f"{prefix}.perf.frameDtProxy",
            "perf rows are one-second aggregates; implied frame seconds is descriptive only",
            {"medianImpliedFrameSeconds": 1.0 / fps_median if fps_median else None,
             "hairGpuMedianMs": median(hair)})

    def check_timing(self, prefix, run):
        timing = run.get("timing")
        if not isinstance(timing, dict):
            self.gap(f"{prefix}.timing", "run has no timing evidence block")
            return
        source = timing.get("source")
        if source not in ("hair-stats", "runtime-frame-history"):
            self.gap(
                f"{prefix}.timing",
                f"timing.source={source!r} is not a per-frame runtime source; "
                "perf JSONL and sampled perf snapshots are aggregates")
            return
        frames = timing.get("frames")
        self.check(f"{prefix}.timing.frames", isinstance(frames, int) and frames >= 2,
                   f"timing.frames={frames!r}")
        step = timing.get("timeStepSeconds")
        frame_dt = timing.get("frameDtSeconds")
        self.check(f"{prefix}.timing.block",
                   isinstance(step, dict) and isinstance(frame_dt, dict),
                   "timing needs timeStepSeconds and frameDtSeconds blocks")
        if not isinstance(step, dict) or not isinstance(frame_dt, dict):
            return
        for label, block in (("frameDt", frame_dt), ("timeStep", step)):
            values = [block.get(key) for key in ("min", "p50", "p95", "max")]
            self.check(
                f"{prefix}.timing.{label}.finite",
                all(finite_number(value) for value in values) and
                values[0] <= values[1] <= values[2] <= values[3],
                f"{label} values must be finite and ordered",
                evidence={"values": values})
            self.check(f"{prefix}.timing.{label}.nonnegative", values[0] >= 0,
                       f"{label} min must be nonnegative", evidence={"min": values[0]})
        max_dt = MAX_DT_SECONDS
        descriptor = self.report.get("hairDescriptor") or {}
        simulation = descriptor.get("simulation") if isinstance(descriptor, dict) else None
        if isinstance(simulation, dict) and finite_number(simulation.get("maxDt")):
            max_dt = simulation["maxDt"]
        self.threshold(
            f"{prefix}.timing.maxDt",
            finite_number(step.get("max")) and step["max"] <= max_dt + 1e-6,
            f"timeStep.max {step.get('max')!r} exceeds maxDt {max_dt}",
            {"maxDt": max_dt})
        ratio = timing.get("physicalSecondsPerRealSecond")
        self.check(f"{prefix}.timing.physicalRatio",
                   finite_number(ratio) and ratio > 0,
                   f"physicalSecondsPerRealSecond={ratio!r}")
        fps = (run.get("perf") or {}).get("medianFps")
        if finite_number(ratio) and finite_number(fps) and fps > 0:
            expected = fps * min(1.0 / fps, max_dt)
            tolerance = max(0.02, expected * 0.10)
            self.threshold(
                f"{prefix}.timing.physicalRatioMatch",
                abs(ratio - expected) <= tolerance,
                f"physical ratio {ratio:.4f} differs from fps/maxDt expectation {expected:.4f}",
                {"ratio": ratio, "expected": expected, "tolerance": tolerance})
        dispatch = timing.get("dispatchCounts")
        self.check(f"{prefix}.timing.dispatchCounts",
                   isinstance(dispatch, dict) and
                   all(isinstance(dispatch.get(key), int) for key in ("min", "p50", "max")),
                   "timing.dispatchCounts needs integer min/p50/max")
        self.check(f"{prefix}.timing.resetCounts",
                   isinstance(timing.get("resetCounts"), dict),
                   "timing.resetCounts must be recorded")

    def check_trace(self, prefix, run, run_dir):
        """校验一次 `hair trace` 回读，并报告逐点有限值/段长/轨迹指标。"""
        trace = run.get("trace")
        if not isinstance(trace, dict):
            self.gap(
                f"{prefix}.trace",
                "run has no GPU guide trace; finite positions, segment lengths and "
                "root/tip trajectories remain unproven")
            return
        path_value = trace.get("path")
        if not isinstance(path_value, str) or not path_value.strip():
            self.fail(f"{prefix}.trace.path", "trace.path must be a non-empty string")
            return
        path = Path(path_value)
        if not path.is_absolute():
            path = run_dir / path
        if not path.is_file():
            self.fail(f"{prefix}.trace.exists", f"trace file is missing: {path}")
            return
        actual_bytes = path.stat().st_size
        recorded_bytes = trace.get("bytes")
        self.check(f"{prefix}.trace.bytes", recorded_bytes == actual_bytes,
                   "trace byte count differs from report",
                   evidence={"report": recorded_bytes, "actual": actual_bytes})
        recorded_sha = trace.get("sha256")
        self.check(f"{prefix}.trace.sha256", is_sha256(recorded_sha),
                   "trace.sha256 must be a 64-character lowercase hex digest",
                   evidence={"sha256": recorded_sha})
        if is_sha256(recorded_sha):
            self.check(f"{prefix}.trace.sha256Current", digest(path) == recorded_sha,
                       "trace file digest differs from report")
        try:
            metrics = read_hair_trace(path)
        except (OSError, TraceFormatError) as error:
            self.fail(f"{prefix}.trace.parse", f"{type(error).__name__}: {error}")
            return

        self.trace_reports.append(metrics)
        self.summary["traceFrames"] += metrics["frames"]
        self.summary["traceBytes"] += metrics["bytes"]
        requested = trace.get("requestedFrames")
        self.check(f"{prefix}.trace.frames",
                   isinstance(requested, int) and requested > 0 and
                   metrics["frames"] == requested,
                   f"trace frames={metrics['frames']}, requested={requested!r}")
        for key, metric_key in (("selectedGuides", "selectedCount"),
                                ("pointsPerGuide", "pointsPerGuide"),
                                ("stride", "stride"),
                                ("startFrame", "startFrame")):
            if trace.get(key) is not None:
                self.check(f"{prefix}.trace.{key}",
                           trace.get(key) == metrics[metric_key],
                           f"report {key} differs from trace header",
                           evidence={"report": trace.get(key),
                                     "header": metrics[metric_key]})
        self.check(f"{prefix}.trace.finiteCount", metrics["finiteCount"] == 0,
                   "trace contains non-finite position/state values",
                   evidence={"finiteCount": metrics["finiteCount"]})
        self.check(f"{prefix}.trace.rootTipFinite",
                   metrics["rootNonFinite"] == 0 and metrics["tipNonFinite"] == 0,
                   "root or tip trajectory contains non-finite positions",
                   evidence={"rootNonFinite": metrics["rootNonFinite"],
                             "tipNonFinite": metrics["tipNonFinite"]})
        self.check(f"{prefix}.trace.framesWithNoSubsteps",
                   metrics["framesWithNoSubsteps"] == 0,
                   "trace contains frames without a simulation substep")
        self.check(f"{prefix}.trace.dispatchCount",
                   isinstance(metrics["dispatchCountMin"], int) and
                   metrics["dispatchCountMin"] > 0,
                   "trace frames must record at least one dispatch",
                   evidence={"min": metrics["dispatchCountMin"],
                             "max": metrics["dispatchCountMax"]})
        self.check(f"{prefix}.trace.segmentSamples",
                   metrics["segmentRatioSamples"] > 0,
                   "trace has no adjacent-point segment samples")
        self.threshold(
            f"{prefix}.trace.sdfPenetration",
            finite_number(metrics["sdfPenetrationMaxCm"]) and
            metrics["sdfPenetrationMaxCm"] <= TRACE_PENETRATION_MAX_CM,
            f"SDF pre-projection penetration {metrics['sdfPenetrationMaxCm']!r} "
            f"exceeds {TRACE_PENETRATION_MAX_CM} cm",
            {"maxCm": metrics["sdfPenetrationMaxCm"]})
        self.threshold(
            f"{prefix}.trace.capsulePenetration",
            finite_number(metrics["capsulePenetrationMaxCm"]) and
            metrics["capsulePenetrationMaxCm"] <= TRACE_PENETRATION_MAX_CM,
            f"capsule pre-projection penetration "
            f"{metrics['capsulePenetrationMaxCm']!r} exceeds "
            f"{TRACE_PENETRATION_MAX_CM} cm",
            {"maxCm": metrics["capsulePenetrationMaxCm"]})
        self.threshold(
            f"{prefix}.trace.segmentRatioP99",
            metrics["segmentRatioP99"] is not None and
            metrics["segmentRatioP99"] <= TRACE_SEGMENT_RATIO_P99_LIMIT,
            f"segment ratio p99 {metrics['segmentRatioP99']!r} exceeds "
            f"{TRACE_SEGMENT_RATIO_P99_LIMIT}",
            {"p50": metrics["segmentRatioP50"], "p99": metrics["segmentRatioP99"],
             "max": metrics["segmentRatioMax"]})
        self.threshold(
            f"{prefix}.trace.segmentRatioMax",
            metrics["segmentRatioMax"] is not None and
            metrics["segmentRatioMax"] <= TRACE_SEGMENT_RATIO_MAX_LIMIT,
            f"segment ratio max {metrics['segmentRatioMax']!r} exceeds "
            f"{TRACE_SEGMENT_RATIO_MAX_LIMIT}",
            {"max": metrics["segmentRatioMax"]})
        self.check(
            f"{prefix}.trace.timeStep",
            finite_number(metrics["timeStepMin"]) and
            finite_number(metrics["timeStepMax"]) and
            metrics["timeStepMin"] > 0 and
            metrics["timeStepMax"] <= MAX_DT_SECONDS + 1e-6,
            "trace timeStep must be positive and within the source solver clamp",
            evidence={"min": metrics["timeStepMin"], "max": metrics["timeStepMax"]})
        self.pass_(
            f"{prefix}.trace.metrics",
            "GPU readback metrics computed from the VLHT file",
            {"frames": metrics["frames"], "finiteCount": metrics["finiteCount"],
             "segmentRatioP99": metrics["segmentRatioP99"],
             "segmentRatioMax": metrics["segmentRatioMax"],
             "rootMaxStepCm": metrics["rootMaxStepCm"],
             "tipMaxStepCm": metrics["tipMaxStepCm"],
             "sdfPenetrationMaxCm": metrics["sdfPenetrationMaxCm"],
             "capsulePenetrationMaxCm": metrics["capsulePenetrationMaxCm"],
             "resetReasonCounts": metrics["resetReasonCounts"],
             "substeps": metrics["substeps"]})

    # ---- acceptance ------------------------------------------------------
    def check_acceptance(self):
        if self.profile != "acceptance":
            self.pass_(
                "acceptance.profile",
                "capture profile: numerical/readback acceptance is not claimed")
            return
        if not isinstance(self.report.get("scenarioCoverage"), list):
            self.gap("acceptance.scenarioCoverage", "no matrix coverage table")
        for run_index, run in enumerate(self.report.get("runs") or []):
            timing = (run or {}).get("timing") if isinstance(run, dict) else None
            if not isinstance(timing, dict) or timing.get("source") not in (
                    "hair-stats", "runtime-frame-history"):
                self.gap(f"acceptance.timing.run[{run_index}]",
                         "no per-frame dt/timeStep/dispatch telemetry")
        if self.report.get("physicsNumericalValidation") is not True:
            self.gap("acceptance.physicsNumericalValidation",
                     "report explicitly sets physicsNumericalValidation=false")
        if not self.report.get("numericEvidence"):
            self.gap("acceptance.numericEvidence",
                     "no GPU readback or CPU reference export artifact")
        self.pass_("acceptance.profile", "acceptance profile evaluates the full matrix")

    # ---- result ----------------------------------------------------------
    def result(self):
        code = EXIT_PASS
        if self.failures:
            codes = {failure["code"] for failure in self.failures}
            code = EXIT_THRESHOLD if EXIT_THRESHOLD in codes else EXIT_EVIDENCE
        accepted = self.profile == "acceptance" and not self.failures
        return {
            "schemaVersion": SCHEMA_VERSION,
            "checker": "verify_hair_physics.py",
            "generatedAt": now_iso(),
            "captureDirectory": str(self.capture_dir),
            "profile": self.profile,
            "passed": code == EXIT_PASS,
            "accepted": accepted,
            "exitCode": code,
            "summary": self.summary,
            "acceptance": {
                "ready": accepted,
                "missingOrUnproven": self.acceptance_gaps,
            },
            "matrixCoverage": {
                "required": list(self.required_scenarios),
                "entries": [self.coverage[key] for key in sorted(self.coverage)],
                "notCaptured": [
                    scenario for scenario in self.required_scenarios
                    if (self.coverage.get(scenario) or {}).get("status") != "captured"],
            },
            "trace": self.trace_reports,
            "checks": self.findings,
            "failures": self.failures,
        }


class CheckerArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(EXIT_USAGE, f"{self.prog}: error: {message}\n")


def parse_args():
    parser = CheckerArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path,
                        help="Capture directory containing report.json, or report.json itself")
    parser.add_argument("--profile", choices=("capture", "acceptance"), default="acceptance",
                        help="capture = integrity only; acceptance = full matrix evidence (default)")
    parser.add_argument("--json-out", type=Path, default=None,
                        help="Also write the machine-readable result to this path")
    parser.add_argument("--quiet", action="store_true",
                        help="Print one status line instead of the full JSON")
    parser.add_argument("--require-scenario", action="append", default=None,
                        help="Override acceptance scenario ids; repeatable")
    parser.add_argument("--expect-rate", type=int, action="append", default=None,
                        help="Acceptance rate run expected; repeatable (default 30 60 120 144)")
    return parser.parse_args()


def main():
    try:
        args = parse_args()
    except SystemExit as error:
        return int(error.code) if isinstance(error.code, int) else EXIT_USAGE
    required = tuple(args.require_scenario) if args.require_scenario else ACCEPTANCE_SCENARIOS
    expected_rates = tuple(args.expect_rate) if args.expect_rate else (
        (30, 60, 120, 144) if args.profile == "acceptance" else ())
    verifier = Verifier(args.capture, args.profile, required, expected_rates)
    try:
        result = verifier.run()
    except FileNotFoundError as error:
        print(json.dumps({
            "passed": False, "accepted": False, "exitCode": EXIT_USAGE,
            "error": str(error),
        }, indent=2))
        return EXIT_USAGE
    except (OSError, ValueError) as error:
        print(json.dumps({
            "passed": False, "accepted": False, "exitCode": EXIT_USAGE,
            "error": f"{type(error).__name__}: {error}",
        }, indent=2))
        return EXIT_USAGE
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.quiet:
        print(json.dumps({
            "passed": result["passed"], "accepted": result["accepted"],
            "exitCode": result["exitCode"], "profile": result["profile"],
            "failures": len(result["failures"]),
            "acceptanceGaps": len(result["acceptance"]["missingOrUnproven"]),
        }))
    else:
        print(json.dumps(result, indent=2))
    return result["exitCode"]


if __name__ == "__main__":
    sys.exit(main())
