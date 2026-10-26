"""External Female hair interaction smoke captures through production Console.

Run serially with other Vulkan sessions because startup publishes shader/spv.
Example:
  python tool/hair/capture_hair_physics.py --output artifacts/hair-physics \
      --build-directory build-rel --rates 30 60 120
Use --duration 600 --rates 60 for an optional longer continuous Walk session.
Use --rates 0 for an uncapped variable-dt run; use --pause-probes / --settle-probes
to sample a paused or idle session over time. Images and perf data are smoke
evidence, not numerical collision/length validation.
Use --placement-probes for the common-actor placement sequence (idle -> +100 cm
translation -> 90 degrees Y rotation -> teleport -> sweep), driven only by the
normal Console `mesh transform/offset/sweep` commands. It is off by default so
the 30/60/120, 4 s Walk, 9-image sequence is unchanged when the flag is absent.
"""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import math
import msvcrt
from pathlib import Path
import platform
import re
import shutil
import statistics
import struct
import subprocess
import time

# 报告模式版本：后续字段只做追加；verify_hair_physics.py 按版本决定严格程度。
SCHEMA_VERSION = 1
TOOL_VERSION = "1.2.0"

# 验证矩阵中每个场景的当前工具覆盖状态。这里只描述采集器能做到什么，
# 不把未采集的项写成通过；verify_hair_physics.py 会原样读取并报告缺口。
# placement 三项在运行时由 scenario_coverage() 按是否启用 --placement-probes 改写，
# 因为“有命令可驱动”与“本次真的采到”是两件事。
SCENARIO_COVERAGE = [
    {"id": "motion.placement.translation", "status": "partial",
     "reason": "正常 Console `mesh offset/transform` 可驱动共同 Actor 平移；默认序列未启用，"
               "且图片只证明可见性"},
    {"id": "motion.placement.rotation-y", "status": "partial",
     "reason": "`mesh transform ... 0 90 0` 可绕 Y 转身；默认序列未启用，"
               "且没有逐点角误差读回"},
    {"id": "motion.placement.teleport", "status": "partial",
     "reason": "`mesh offset` 大位移可做传送；默认序列未启用，复位事件由采集器记录"},
    {"id": "motion.head-turn", "status": "unsupported",
     "reason": "正式场景只有 Idle/Walk 两个 clip，没有快速转头夹具或运行时姿态命令"},
    {"id": "measure.guide-trajectory", "status": "unsupported",
     "reason": "没有 guide 逐点位置读回；截图不能给出根点/发梢轨迹"},
    {"id": "time.rates", "status": "captured",
     "reason": "--frame-limit 支持 30/60/120/144 Hz，采集器按 --rates 逐档运行"},
    {"id": "time.variable-dt", "status": "partial",
     "reason": "--rates 0 走不限速启动，但当前没有每帧 dt 记录"},
    {"id": "time.per-frame-dt", "status": "unsupported",
     "reason": "perf JSONL 每秒一行，fps/cpuMs 是聚合值；没有逐帧 dt 直方图"},
    {"id": "time.dispatch-count", "status": "unsupported",
     "reason": "dispatch 日志只在首次工作帧输出一次，passes=9 是表大小而非每帧 dispatch 数"},
    {"id": "time.physical-time-ratio", "status": "partial",
     "reason": "可用实际 fps 推算，但缺逐帧 timeStep，不能证明每真实秒物理时间"},
    {"id": "time.reset-events", "status": "captured",
     "reason": "ResetPositions 事件带 reason/frame/identity，按次记录"},
    {"id": "time.pause-stepping", "status": "partial",
     "reason": "可拍动画 pause 前后并查 reset；没有 step 计数，不能证明发丝继续推进"},
    {"id": "vsp.on-off", "status": "unsupported",
     "reason": "VSP 系数只来自 SM 作者资产，Console 没有运行时开关"},
    {"id": "collision.contact-views", "status": "partial",
     "reason": "固定机位侧/背接触图可拍；胶囊/SDF 不可在运行时单独关闭，图片不算穿透深度"},
    {"id": "measure.segment-length", "status": "unsupported",
     "reason": "需要逐点 current/previous/prevPrev 或 rest length 读回"},
    {"id": "measure.penetration", "status": "unsupported",
     "reason": "需要胶囊/SDF 距离与 guide 点位置；截图不能量化穿透"},
    {"id": "measure.contact-jitter", "status": "unsupported",
     "reason": "需要静止接触段的位置时间序列；当前只有图片和每秒聚合 perf"},
    {"id": "measure.finite-values", "status": "unsupported",
     "reason": "没有位置/速度缓冲区读回，无法证明无 NaN/Inf 或段长上界"},
]


from PIL import Image, ImageChops, ImageDraw, ImageStat


SCENE = "Maps/SC_tressfx_female/SC_tressfx_female_hair_animated.json"
HAIR = "Maps/SC_tressfx_female/Meshes/TF_female_hair_skinned.json"
IDLE = "M_Neutral_Stand_Idle_Loop"
WALK = "M_Neutral_Walk_Loop_F"
WALK_CAMERA = "camera pose 0 151 950 0 151 400"

# 场景 JSON 里的两个 mesh 对象身份；共同 Actor placement 必须同时移动两者，
# 否则 sourceToHair 会变，验的就不再是“角色整体移动时的发丝惯性”。
CHARACTER_OBJECT = "FemaleCharacter"
HAIR_OBJECT = "Female_hair_skinned"
# placement 探针的固定机位：离原点 900 cm，场景 PortraitCamera 的 fov=38（水平），
# far_clip=1000。因此 actor 到相机距离必须 <1000 cm，平移只能沿 X 且 |x| 不超过约
# 250 cm，否则会既出画又越过 far plane，图片就不再是可见性证据。
PLACEMENT_CAMERA = "camera pose 0 151 900 0 151 0"
PLACEMENT_TRANSLATE_CM = 100.0
PLACEMENT_ROTATION_Y_DEGREES = 90.0
PLACEMENT_TELEPORT_DELTA_CM = (150.0, 0.0, 0.0)
PLACEMENT_SWEEP_DELTA_CM = (-300.0, 0.0, 0.0)
# 每次 Console 注入本身要花 ~0.5 s（另起 pwsh），所以 sweep 必须明显长于注入开销，
# 否则“中间帧”会在 sweep 结束后才拍到。报告里用 `sweepMidCapturedDuringMotion` 断言。
PLACEMENT_SWEEP_SECONDS = 2.0


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_log(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


@contextmanager
def runtime_lock(root):
    lock_path = root / "artifacts/hair-physics-runtime.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError("Another hair capture owns the runtime lock") from error
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def ensure_alive(process):
    if process.poll() is not None:
        raise RuntimeError(f"Engine exited early: {process.returncode}")


def wait_seconds(process, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ensure_alive(process)
        time.sleep(min(0.25, max(0, deadline - time.monotonic())))


def wait_bmp(process, path, log_path, timeout=180):
    deadline = time.monotonic() + timeout
    previous_size = None
    stable = 0
    while time.monotonic() < deadline:
        ensure_alive(process)
        if path.exists() and path.stat().st_size >= 54:
            with path.open("rb") as stream:
                header = stream.read(54)
            if header[:2] != b"BM":
                raise RuntimeError(f"Invalid BMP magic: {path}")
            offset = struct.unpack_from("<I", header, 10)[0]
            width, height = struct.unpack_from("<ii", header, 18)
            bits = struct.unpack_from("<H", header, 28)[0]
            if width <= 0 or height == 0 or bits != 24:
                raise RuntimeError(f"Unsupported BMP shape/format: {path}")
            expected = offset + ((width * 3 + 3) & ~3) * abs(height)
            size = path.stat().st_size
            stable = stable + 1 if size == previous_size == expected else 0
            previous_size = size
            # 完整载荷、连续稳定和引擎完成日志一起作为文件可读条件。
            if stable >= 2 and "Screenshot saved:" in read_log(log_path):
                with Image.open(path) as image:
                    result = image.convert("RGB")
                    result.load()
                if max(ImageStat.Stat(result).var) < 1:
                    raise RuntimeError(f"Screenshot is blank: {path}")
                return result
        time.sleep(0.1)
    raise TimeoutError(f"Screenshot did not finish: {path}")


def now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def scenario_coverage(placement_probes):
    """按本次启动参数改写覆盖表：只有真的跑了 placement 探针才标 captured。

    静态表描述“采集器具备什么”，不描述“这次采到了什么”；把两者混在一起
    会让默认运行误报已覆盖 placement。默认运行仍把三项记为 partial。
    """
    coverage = [dict(entry) for entry in SCENARIO_COVERAGE]
    for entry in coverage:
        if not entry["id"].startswith("motion.placement."):
            continue
        if placement_probes:
            entry["status"] = "captured"
            entry["reason"] = (
                "正常 Console `mesh transform/offset/sweep` 已驱动两个共同 Actor；"
                "本次报告 placement 段记录命令回执、reset 事件、dispatch 与图像。"
                "仍没有逐点轨迹读回，不能据此给出惯性/过冲数值结论")
        else:
            entry["status"] = "partial"
            entry["reason"] = (
                "Console 已有 `mesh transform/offset/sweep`，但本次未启用 "
                "--placement-probes，未采集 placement 序列")
    return coverage


def parse_reset_events(text):
    """解析 [TressFX] ResetPositions 事件；字段顺序固定，不做模糊匹配。"""
    pattern = re.compile(
        r'\[TressFX\] ResetPositions: object="(?P<object>[^"]*)" '
        r"reason=(?P<reason>[a-z-]+) playbackIdentity=(?P<identity>\d+) "
        r"discontinuity=(?P<discontinuity>\d+) frame=(?P<frame>\d+)")
    events = []
    for line in text.splitlines():
        match = pattern.search(line)
        if match:
            entry = match.groupdict()
            events.append({
                "object": entry["object"], "reason": entry["reason"],
                "playbackIdentity": int(entry["identity"]),
                "discontinuity": int(entry["discontinuity"]),
                "frame": int(entry["frame"]),
            })
    return events


def parse_dispatch(text):
    """解析首次 GPU dispatch 诊断；passes 是表大小，不是本帧 dispatch 次数。"""
    pattern = re.compile(
        r"\[TressFX\] GPU guide simulation dispatch: objects=(?P<objects>\d+) "
        r"guides=(?P<guides>\d+) points=(?P<points>\d+) "
        r"passes=(?P<passes>\d+) sdfObjects=(?P<sdf>\d+)")
    for line in text.splitlines():
        match = pattern.search(line)
        if match:
            values = {key: int(value) for key, value in match.groupdict().items()}
            return {
                "objects": values["objects"], "guides": values["guides"],
                "points": values["points"], "passes": values["passes"],
                "sdfObjects": values["sdf"], "line": line,
                "interpretation": "passes 是 GPU pass 表大小；该行只在首个工作帧输出一次",
            }
    return None


def last_perf_json(text):
    """取 Console `perf json` 的最后一行完整 JSON；不把截图/播放日志混进来。

    命令输出由 GT 执行端异步打印，通常落在已经打印的 `debug> ` 之后；这里只剥掉
    该提示前缀，不改变 JSON 本身的解析条件。
    """
    result = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("debug> "):
            line = line[len("debug> "):].lstrip()
        if not line.startswith('{"fps"'):
            continue
        try:
            candidate = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, dict) and "fps" in candidate:
            result = candidate
    return result


def bmp_info(path):
    """读取 BMP 头，返回宽高/位深/预期字节数；格式不符时抛错。"""
    with path.open("rb") as stream:
        header = stream.read(54)
    if len(header) < 54 or header[:2] != b"BM":
        raise RuntimeError(f"Invalid BMP magic: {path}")
    offset = struct.unpack_from("<I", header, 10)[0]
    width, height = struct.unpack_from("<ii", header, 18)
    bits = struct.unpack_from("<H", header, 28)[0]
    if width <= 0 or height == 0 or bits != 24:
        raise RuntimeError(f"Unsupported BMP shape/format: {path}")
    expected = offset + ((width * 3 + 3) & ~3) * abs(height)
    return {
        "width": width, "height": abs(height), "topDown": height < 0,
        "bitsPerPixel": bits, "declaredBytes": expected,
        "actualBytes": path.stat().st_size,
    }


class Session:
    def __init__(self, root, output, process, report):
        self.root = root
        self.output = output
        self.process = process
        self.report = report
        self.started = time.monotonic()
        self.log_path = output / "stdout.log"

    def command(self, text):
        ensure_alive(self.process)
        before = time.monotonic()
        result = subprocess.run([
            shutil.which("pwsh") or "pwsh", "-NoProfile", "-File",
            str(self.root / "tool/validation/console_inject.ps1"),
            "-InjectProcessId", str(self.process.pid), "-InjectLine", text,
        ], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"Console injection failed: {result.stdout} {result.stderr}")
        self.report["commands"].append({
            "lines": text.split("\r"), "submittedSeconds": before - self.started,
            "injectionSeconds": time.monotonic() - before,
        })

    def snapshot(self, name, expected_clip=None, playing=None, commands=(), perf_probe=False):
        offset = len(read_log(self.log_path))
        path = self.output / f"{name}.bmp"
        # 同一次注入提交动作/查询/截图，避免多个 PowerShell 启动把首帧问题等到收敛后才采集。
        # perf json 只追加在查询前，默认关闭以保持旧采集序列不变。
        query = ["perf json"] if perf_probe else []
        self.command("\r".join([
            *commands, *query, "animation get", f"screenshot {path.as_posix()}"]))
        image = wait_bmp(self.process, path, self.log_path)
        png_path = self.output / f"{name}.png"
        image.save(png_path)
        section = read_log(self.log_path)[offset:]
        lines = [line for line in section.splitlines() if "Animation[" in line]
        if expected_clip and not any(
                f"id={expected_clip}" in line and
                (playing is None or f"playing={int(playing)}" in line) for line in lines):
            raise RuntimeError(f"{name}: missing requested playback evidence: {lines}")
        reset_events = parse_reset_events(section)
        entry = {
            "name": name, "capturedSeconds": time.monotonic() - self.started,
            "size": list(image.size), "bmpSha256": digest(path),
            "pngSha256": digest(png_path),
            "bmpBytes": path.stat().st_size, "pngBytes": png_path.stat().st_size,
            "bmpHeader": bmp_info(path),
            "playback": lines,
            # 该截图窗口内的离散事件；窗口按截图前 stdout 偏移切分。
            "resetEvents": reset_events,
            "resetReasonCounts": {
                reason: sum(event["reason"] == reason for event in reset_events)
                for reason in ("initial-state", "first-pose", "playback-changed",
                               "pose-discontinuity", "pose-recovered")
            },
            "dispatchEvidence": [
                line for line in section.splitlines()
                if "GPU guide simulation dispatch:" in line],
            "sdfEvidence": [
                line for line in section.splitlines()
                if "SDF collision buffer:" in line or "SDF domain:" in line],
        }
        if perf_probe:
            entry["perfSnapshot"] = last_perf_json(section)
        self.report["captures"].append(entry)
        print(f"{self.report['rateHz']} Hz: {name}", flush=True)
        return image


def perf_summary(path):
    rows = [json.loads(line) for line in read_log(path).splitlines() if line.strip()]
    hair_ms = [
        entry["ms"] for row in rows for entry in row["passes"]
        if entry["name"] == "hairSimulation" and entry["ms"] is not None
    ]
    fps = [row["fps"] for row in rows if row.get("fps") is not None]
    cpu = [row["cpuMs"] for row in rows if row.get("cpuMs") is not None]
    gpu_total = [row["gpuTotalMs"] for row in rows if row.get("gpuTotalMs") is not None]
    return {
        "samples": len(rows),
        "lines": len(read_log(path).splitlines()),
        "validGpuSamples": sum(bool(row.get("valid")) for row in rows),
        "gpuTotalSamples": len(gpu_total),
        "hairGpuSamples": len(hair_ms),
        "medianFps": statistics.median(fps) if fps else None,
        "minFps": min(fps) if fps else None,
        "maxFps": max(fps) if fps else None,
        "medianCpuMs": statistics.median(cpu) if cpu else None,
        "medianHairGpuMs": statistics.median(hair_ms) if hair_ms else None,
        "medianGpuTotalMs": statistics.median(gpu_total) if gpu_total else None,
        "sha256": digest(path) if path.exists() else None,
        # 每秒聚合行不是逐帧 dt；这里仅记录可由 fps 反推的帧间隔中位数。
        "impliedMedianFrameSeconds": 1.0 / statistics.median(fps) if fps else None,
    }


def contact_sheet(output, images):
    width = 640
    cells = []
    for name, image in images.items():
        scaled = image.copy()
        scaled.thumbnail((width, 360))
        cell = Image.new("RGB", (width, 388), (25, 27, 29))
        cell.paste(scaled, ((width - scaled.width) // 2, 28))
        ImageDraw.Draw(cell).text((12, 8), name, fill=(240, 240, 240))
        cells.append(cell)
    result = Image.new("RGB", (width * 3, 388 * math.ceil(len(cells) / 3)), (25, 27, 29))
    for index, cell in enumerate(cells):
        result.paste(cell, ((index % 3) * width, (index // 3) * 388))
    result.save(output / "contact-sheet.png")


TRACE_RE = re.compile(
    r'\[TressFX\] Hair trace complete: file="(?P<file>[^"]*)" '
    r"frames=(?P<frames>\d+) requestedFrames=(?P<requested>\d+) "
    r"selectedGuides=(?P<selected>\d+) pointsPerGuide=(?P<points>\d+) "
    r"stride=(?P<stride>\d+) bytes=(?P<bytes>\d+)")


def wait_trace_complete(process, log_path, trace_path, timeout=180):
    """等待一次性 `hair trace` 的完成摘要行；失败/拒绝立即抛错。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ensure_alive(process)
        text = read_log(log_path)
        match = TRACE_RE.search(text)
        if match and trace_path.is_file():
            return match, text
        for failure in (
                "[TressFX] Hair trace failed",
                "[TressFX] Hair trace finished with no captured frames",
                "[TressFX] Hair trace rejected"):
            if failure in text:
                raise RuntimeError(f"hair trace failed: {failure}")
        time.sleep(0.25)
    raise RuntimeError(f"hair trace did not complete within {timeout} s")


def apply_trace_results(report):
    """把成功回读的 trace 结果提升为报告级数值证据与矩阵覆盖。"""
    traces = [run.get("trace") for run in report.get("runs", [])
              if isinstance(run.get("trace"), dict)]
    if not traces:
        return
    first = traces[0]
    report["physicsNumericalValidation"] = True
    report["numericEvidence"] = {
        "kind": "gpu-readback",
        "format": "VLHT",
        "path": first["path"],
        "sha256": first["sha256"],
        "frames": first["frames"],
        "selectedGuides": first["selectedGuides"],
        "pointsPerGuide": first["pointsPerGuide"],
        "interpretation": "选中 guide 子集的只读 GPU 位置回读；不是全量 40k guides",
    }
    for entry in report.get("scenarioCoverage", []):
        if entry.get("id") not in (
                "measure.guide-trajectory", "measure.segment-length",
                "measure.finite-values"):
            continue
        entry["status"] = "captured"
        entry["reason"] = (
            "本次启用 --trace：`hair trace` 从 device-local SSBO 只读回读"
            "选中 guide 的 current/previous/prevPrev，报告与 verify_hair_physics.py "
            "据此计算有限值、段长比与根点/发梢轨迹")


def run_placement_probes(session, images, perf_probes):
    """共同 Actor placement 序列：idle → +100 cm 平移 → 绕 Y 90° → teleport → sweep。

    只通过正常 Console `mesh` 命令驱动；每个动作把两个 identity 的命令放进同一次
    注入，让角色与发丝尽量落在同一个 GT 帧。位移量来自 Console 回执日志，不是逐点
    读回；图片只用于确认“画面上确实移动了”。

    placement 窗口内先 `animation pause`：Idle 是带根运动的循环 clip，每 ~10 s 的
    loop wrap 会自己推进 discontinuity 并触发 `pose-discontinuity` reset。冻结姿态后
    该干扰消失，窗口内任何 reset 都能归因到 placement 本身。发丝仍按风和约束步进。
    """
    log_path = session.log_path
    session.command(PLACEMENT_CAMERA)
    images["placement-idle"] = session.snapshot(
        "placement-idle", IDLE, True, perf_probe=perf_probes)
    # 冻结骨骼姿态，只让 placement 和历史换基产生影响；pause 本身不改变 pose 身份。
    session.command("animation pause")
    wait_seconds(session.process, 0.5)
    # 只统计 idle 之后的窗口：首帧姿态 reset 属于启动证据，不是 placement 触发。
    window_start = len(read_log(log_path))

    translation = f"{PLACEMENT_TRANSLATE_CM:g}"
    rotation = f"{PLACEMENT_ROTATION_Y_DEGREES:g}"
    session.command("\r".join([
        f"mesh offset {CHARACTER_OBJECT} {translation} 0 0",
        f"mesh offset {HAIR_OBJECT} {translation} 0 0",
    ]))
    wait_seconds(session.process, 0.5)
    images["placement-translated"] = session.snapshot(
        "placement-translated", IDLE, False, perf_probe=perf_probes)

    session.command("\r".join([
        f"mesh transform {CHARACTER_OBJECT} {translation} 0 0 0 {rotation} 0",
        f"mesh transform {HAIR_OBJECT} {translation} 0 0 0 {rotation} 0",
    ]))
    wait_seconds(session.process, 0.5)
    images["placement-rotated"] = session.snapshot(
        "placement-rotated", IDLE, False, perf_probe=perf_probes)

    teleport_x, teleport_y, teleport_z = PLACEMENT_TELEPORT_DELTA_CM
    session.command("\r".join([
        f"mesh offset {CHARACTER_OBJECT} {teleport_x:g} {teleport_y:g} {teleport_z:g}",
        f"mesh offset {HAIR_OBJECT} {teleport_x:g} {teleport_y:g} {teleport_z:g}",
    ]))
    wait_seconds(session.process, 0.5)
    images["placement-teleport"] = session.snapshot(
        "placement-teleport", IDLE, False, perf_probe=perf_probes)

    # sweep 是显式命令驱动的有限时长线性推进；mid/end 两张图证明中间状态和终点。
    sweep_x, sweep_y, sweep_z = PLACEMENT_SWEEP_DELTA_CM
    session.command(
        f"mesh sweep {CHARACTER_OBJECT} {HAIR_OBJECT} "
        f"{sweep_x:g} {sweep_y:g} {sweep_z:g} {PLACEMENT_SWEEP_SECONDS:g}")
    wait_seconds(session.process, 0.25)
    images["placement-sweep-mid"] = session.snapshot(
        "placement-sweep-mid", IDLE, False, perf_probe=perf_probes)
    wait_seconds(session.process, 1.6)
    images["placement-sweep-end"] = session.snapshot(
        "placement-sweep-end", IDLE, False, perf_probe=perf_probes)
    # 结束时恢复播放，避免把本进程留在“动画暂停”状态；replay 不是采集目标。
    session.command("animation play")

    section = read_log(log_path)[window_start:]
    evidence = [line.strip() for line in section.splitlines()
                if "[Diagnostics]" in line and "Mesh " in line]
    rejections = [line.strip() for line in section.splitlines()
                  if "[Diagnostics][Warning]" in line and "Mesh " in line]
    reset_events = parse_reset_events(section)
    expected = (
        f"Mesh {CHARACTER_OBJECT} offset",
        f"Mesh {HAIR_OBJECT} offset",
        f"Mesh {CHARACTER_OBJECT} transform",
        f"Mesh {HAIR_OBJECT} transform",
        "Mesh sweep started",
        "Mesh sweep finished",
    )
    missing = [prefix for prefix in expected
               if not any(prefix in line for line in evidence)]
    # 注入开销会把“中间帧”推后；只有 mid 的 Screenshot saved 早于 sweep finished
    # 才能证明那张图拍在运动过程中，而不是已经停在终点。
    section_lines = section.splitlines()
    mid_saved = -1
    sweep_finished = -1
    for index, line in enumerate(section_lines):
        if "Screenshot saved" in line and "placement-sweep-mid.bmp" in line:
            mid_saved = index
        if "Mesh sweep finished" in line:
            sweep_finished = index
    sweep_mid_during_motion = 0 <= mid_saved < sweep_finished
    return {
        "mode": "idle-offset-transform-teleport-sweep",
        "camera": PLACEMENT_CAMERA,
        "translationCm": PLACEMENT_TRANSLATE_CM,
        "rotationYDegrees": PLACEMENT_ROTATION_Y_DEGREES,
        "teleportDeltaCm": list(PLACEMENT_TELEPORT_DELTA_CM),
        "sweepDeltaCm": list(PLACEMENT_SWEEP_DELTA_CM),
        "sweepSeconds": PLACEMENT_SWEEP_SECONDS,
        # Idle 循环 clip 每 ~10 s 自己推进 discontinuity 并 reset；冻结姿态后该干扰消失，
        # 窗口内 reset 才能唯一归因到 placement。
        "poseFrozenDuringPlacement": True,
        "evidence": evidence,
        "rejections": rejections,
        "missingEvidence": missing,
        "sweepMidCapturedDuringMotion": sweep_mid_during_motion,
        "resetEvents": reset_events,
        "resetReasonCounts": {
            reason: sum(event["reason"] == reason for event in reset_events)
            for reason in ("initial-state", "first-pose", "playback-changed",
                           "pose-discontinuity", "pose-recovered")
        },
        "dispatchEvidence": [line for line in section.splitlines()
                             if "GPU guide simulation dispatch:" in line],
        "interpretation": (
            "窗口内骨骼姿态已冻结，Idle loop wrap 的 pose-discontinuity 干扰不存在，"
            "因此 ResetPositions 期望为 0；命令回执只证明 World 根 TRS 被写入，"
            "不是逐点轨迹或惯性数值证据"),
    }


def run_rate(root, executable, output, rate, duration, require_reset,
             startup_delay, startup_only, pause_probes=0, pause_probe_interval=1.0,
             settle_probes=0, settle_interval=1.0, perf_probes=False,
             placement_probes=False, trace_frames=0, trace_stride=1024):
    output.mkdir()
    uncapped = rate == 0
    report = {
        "schemaVersion": SCHEMA_VERSION,
        "rateHz": rate, "configuredFrameLimit": None if uncapped else rate,
        "frameLimitMode": "uncapped" if uncapped else "capped",
        "runDirectory": str(output), "completed": False,
        "commands": [], "captures": [],
        "completionMeaning": "Capture/log acquisition only; not a physics or visual acceptance",
        "visualReviewRequired": True,
        "processClosure": "Owned process terminated; not graceful teardown evidence",
        "physicsNumericalValidation": False,
        "requireReset": require_reset,
        "pauseProbesRequested": pause_probes,
        "settleProbesRequested": settle_probes,
        "placementProbesRequested": placement_probes,
        "traceRequested": bool(trace_frames),
        "traceFramesRequested": trace_frames,
        "traceStride": trace_stride,
        "scope": "Initial Idle frames" if startup_only else
                 "Placement probes (Idle, translate, rotate, teleport, sweep)" if placement_probes else
                 "Continuous Idle/Walk, animation pause, seek/clip discontinuities, contact views",
        "limitations": [
            "Commands share one injection, but render scheduling prevents exact first-frame guarantees",
            "Animation pause freezes bones; wind-driven hair may continue",
            "Images cannot establish finite positions, segment lengths or penetration depths",
            "Walk camera looks along authored root motion; inspect actor visibility in each image",
            "Idle/Walk do not replace authored rapid head/placement-teleport fixtures",
            "perf JSONL is one aggregate row per second; it is not a per-frame dt distribution",
            "Placement probes move the actor; images are visibility evidence, not trajectory readback",
            "GPU trace is a read-only selected-guide subset, not the full 40k-guide buffer",
            "The last 1..2 traced frames are flushed on the next matching frame slot or at shutdown",
        ],
    }
    images = {}
    arguments = [str(executable), "--initial-scene", SCENE, "--no-dev-ui"]
    # 0 表示不限速变量 dt；其余正整数仍写入 --frame-limit。
    if not uncapped:
        arguments.extend(("--frame-limit", str(rate)))
    arguments.extend(("--worker-thread-count", "2", "--gpu-stats",
                      "--perf-jsonl", str(output / "perf.jsonl"),
                      "--console-command-delay", str(startup_delay)))
    for command in ("tonemap 3", "bloom strength 0",
                    f"screenshot {(output / 'startup.bmp').as_posix()}"):
        arguments.extend(("--console-command", command))
    try:
        with (output / "stdout.log").open("w", encoding="utf-8") as log, \
                (output / "stderr.log").open("w", encoding="utf-8") as error:
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            process = subprocess.Popen(
                arguments, cwd=root, stdout=log, stderr=error,
                creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
            report["pid"] = process.pid
            session = Session(root, output, process, report)
            try:
                wait_bmp(process, output / "startup.bmp", output / "stdout.log")
                images["idle-continuous"] = session.snapshot(
                    "idle-continuous", IDLE, True, perf_probe=perf_probes)
                if trace_frames:
                    # 正常 Console 一次性诊断命令；不新增 runtime test flag。
                    # 在正式 Female 场景启动完成后注入，随后继续原有采集序列。
                    trace_path = output / "hair-trace.bin"
                    queued = time.monotonic()
                    session.command(
                        f"hair trace {trace_path.as_posix()} {trace_frames} {trace_stride}")
                    match, _ = wait_trace_complete(
                        process, output / "stdout.log", trace_path)
                    values = match.groupdict()
                    requested = int(values["requested"])
                    frames = int(values["frames"])
                    if frames != requested or frames != trace_frames:
                        raise RuntimeError(
                            f"hair trace wrote {frames}/{requested} frames, "
                            f"requested {trace_frames}")
                    if trace_path.stat().st_size != int(values["bytes"]):
                        raise RuntimeError("hair trace summary byte count differs from file")
                    report["trace"] = {
                        "requested": True,
                        "path": str(trace_path),
                        "sha256": digest(trace_path),
                        "bytes": int(values["bytes"]),
                        "frames": frames,
                        "requestedFrames": requested,
                        "selectedGuides": int(values["selected"]),
                        "pointsPerGuide": int(values["points"]),
                        "stride": int(values["stride"]),
                        "summaryLine": match.group(0),
                        "queuedSeconds": queued - session.started,
                        "completedSeconds": time.monotonic() - session.started,
                    }
                # 静置探针在 Walk 之前追加，默认 0 不改变旧序列。
                for index in range(settle_probes):
                    wait_seconds(process, settle_interval)
                    name = f"idle-settle-{index + 1:02d}"
                    images[name] = session.snapshot(
                        name, IDLE, True, perf_probe=perf_probes)
                if placement_probes:
                    report["placement"] = run_placement_probes(
                        session, images, perf_probes)
                if not startup_only and not placement_probes:
                    session.command(WALK_CAMERA)
                    session.command("animation clip 1")
                    session.command("animation play")
                    wait_seconds(process, duration)
                    images["walk-continuous"] = session.snapshot(
                        "walk-continuous", WALK, True, perf_probe=perf_probes)
                    images["walk-paused"] = session.snapshot(
                        "walk-paused", WALK, False, ["animation pause"],
                        perf_probe=perf_probes)
                    # 动画 pause 后的多时刻探针：只为确认没有播放/复位跳变，
                    # 不能证明发丝仍在按某个 dt 步进。
                    for index in range(pause_probes):
                        wait_seconds(process, pause_probe_interval)
                        name = f"walk-paused-{index + 1:02d}"
                        images[name] = session.snapshot(
                            name, WALK, False, perf_probe=perf_probes)
                    images["seek025-early"] = session.snapshot(
                        "seek025-early", WALK, False,
                        ["animation seek 0.25", "camera pose 50 165 260 0 151 50"],
                        perf_probe=perf_probes)
                    wait_seconds(process, 2)
                    images["seek025-later"] = session.snapshot(
                        "seek025-later", WALK, False, perf_probe=perf_probes)
                    images["seek275-early"] = session.snapshot(
                        "seek275-early", WALK, False,
                        ["animation seek 2.75", "camera pose 50 165 760 0 151 550"],
                        perf_probe=perf_probes)
                    images["clip-idle-paused"] = session.snapshot(
                        "clip-idle-paused", IDLE, False,
                        ["animation clip 0", "camera pose 50 165 210 0 151 0"],
                        perf_probe=perf_probes)
                    session.command("animation play")
                    wait_seconds(process, 2)
                    session.command("camera pose 170 161 0 0 151 0")
                    images["side-contact"] = session.snapshot(
                        "side-contact", IDLE, True, perf_probe=perf_probes)
                    session.command("camera pose 0 163 -190 0 149 0")
                    images["back-contact"] = session.snapshot(
                        "back-contact", IDLE, True, perf_probe=perf_probes)
                report["sequenceCompleted"] = True
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=30)
                report["ownedProcessExited"] = process.poll() is not None
                report["processExitCode"] = process.returncode
        text = read_log(output / "stdout.log")
        errors = read_log(output / "stderr.log")
        report["stdoutBytes"] = (output / "stdout.log").stat().st_size
        report["stderrBytes"] = (output / "stderr.log").stat().st_size
        report["vuidCount"] = len(re.findall(r"VUID-", errors + text))
        report["dispatchEvidence"] = [
            line for line in text.splitlines() if "GPU guide simulation dispatch:" in line]
        report["dispatch"] = parse_dispatch(text)
        # dispatch 行只在首个工作帧输出一次，早于 placement 窗口起点；整体证据就是
        # 这次运行确实走 GPU guide 路径的证据，直接沿用到 placement 段。
        if placement_probes and report.get("placement") is not None:
            report["placement"]["dispatchEvidence"] = report["dispatchEvidence"]
        report["sdfEvidence"] = [
            line for line in text.splitlines()
            if "SDF collision buffer:" in line or "SDF domain:" in line]
        report["resetEvidence"] = [
            line for line in text.splitlines()
            if re.search(r"\[(?:Hair|TressFX)\].*(?:reset|Reset)", line)]
        report["resetEvents"] = parse_reset_events(text)
        report["resetReasonCounts"] = {
            reason: sum(event["reason"] == reason for event in report["resetEvents"])
            for reason in ("initial-state", "first-pose", "playback-changed",
                           "pose-discontinuity", "pose-recovered")
        }
        report["perf"] = perf_summary(output / "perf.jsonl")
        # 当前引擎没有逐帧 hair timeStep 通道；保留快照采样作为弱证据，
        # 明确标注 source=not-collected，避免被验收检查器误当作逐帧分布。
        frame_samples = [
            {"name": capture["name"], "capturedSeconds": capture["capturedSeconds"],
             "perf": capture.get("perfSnapshot")}
            for capture in report["captures"] if capture.get("perfSnapshot")]
        report["timing"] = {
            "source": "not-collected",
            "reason": "perf JSONL 每秒一行；没有每帧 frameDelta/timeStep/dispatch 计数。"
                      "perfSnapshot 只是 Console 注入时刻的相邻快照，不是逐帧分布。",
            "frameSamples": frame_samples,
            "speedProxy": {
                "configuredFrameLimitHz": None if uncapped else rate,
                "medianFrameSecondsFromFps": report["perf"].get(
                    "impliedMedianFrameSeconds"),
                "note": "仅为 fps 倒数的聚合代理；不能替代逐帧 dt/timeStep",
            },
        }
        if errors.strip() or report["vuidCount"]:
            raise RuntimeError("Engine stderr/validation diagnostics; inspect stderr.log")
        if "Animation control rejected" in text or "Unknown command" in text:
            raise RuntimeError("A Console command was rejected")
        # placement 命令的拒绝理由由执行端用固定前缀报告；任一出现即视为没被接受。
        if placement_probes and any(
                marker in text for marker in (
                    "Mesh placement rejected", "Mesh transform rejected",
                    "Mesh offset rejected", "Mesh sweep rejected")):
            raise RuntimeError("A mesh placement command was rejected; inspect stdout.log")
        if not report["dispatchEvidence"] or not report["sdfEvidence"]:
            raise RuntimeError("Missing actual GPU guide/SDF path evidence")
        if require_reset and (
                report["resetReasonCounts"]["first-pose"] < 1 or
                (not startup_only and not placement_probes and
                 report["resetReasonCounts"]["pose-discontinuity"] < 4)):
            raise RuntimeError("Missing first-pose or four seek/clip reset events")
        if placement_probes:
            placement = report.get("placement") or {}
            if placement.get("missingEvidence"):
                raise RuntimeError(
                    "Placement evidence missing: " + ", ".join(placement["missingEvidence"]))
            if placement.get("rejections"):
                raise RuntimeError("Placement command was rejected: " + placement["rejections"][0])
            if not placement.get("sweepMidCapturedDuringMotion"):
                raise RuntimeError(
                    "placement-sweep-mid was captured after the sweep had already finished")
            # placement 只改根 TRS，不改 pose 身份；该窗口出现 reset 说明它被误判成姿态跳变。
            if placement.get("resetEvents"):
                raise RuntimeError(
                    "Placement triggered ResetPositions: " +
                    json.dumps(placement["resetEvents"][:3]))
            if not placement.get("dispatchEvidence"):
                raise RuntimeError("Missing GPU guide dispatch evidence during placement")
            if "placement-idle" in images and "placement-translated" in images:
                difference = ImageChops.difference(
                    images["placement-idle"], images["placement-translated"])
                report["placement"]["visualDifferenceOnly"] = {
                    "meanRgbByteDifference": ImageStat.Stat(difference).mean,
                    "interpretation": "Descriptive only; wind and capture latency are uncontrolled",
                }
        if not report["perf"]["medianHairGpuMs"]:
            raise RuntimeError("No measured hairSimulation GPU pass")
        if "seek025-early" in images and "seek025-later" in images:
            difference = ImageChops.difference(images["seek025-early"], images["seek025-later"])
            report["seekVisualDifferenceOnly"] = {
                "meanRgbByteDifference": ImageStat.Stat(difference).mean,
                "interpretation": "Descriptive only; wind/time and capture latency are uncontrolled",
            }
        report["completed"] = True
    except Exception as error:
        report["failure"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if images:
            contact_sheet(output, images)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, default=Path("build-rel"))
    parser.add_argument("--rates", type=int, nargs="+", default=[30, 60, 120],
                        help="Frame caps; 0 means uncapped variable-dt (no --frame-limit)")
    parser.add_argument("--duration", type=float, default=4,
                        help="Continuous Walk seconds at each rate (600 for a longer smoke)")
    parser.add_argument("--require-reset", action="store_true")
    parser.add_argument("--startup-delay", type=int, default=120,
                        help="Frames before startup Console capture (use 5 for a short Debug check)")
    parser.add_argument("--startup-only", action="store_true",
                        help="Capture initial Idle only; omit the interaction sequence")
    parser.add_argument("--pause-probes", type=int, default=0,
                        help="Extra paused snapshots after 'animation pause' (0 keeps the old sequence)")
    parser.add_argument("--pause-probe-interval", type=float, default=1.0,
                        help="Seconds between paused snapshots")
    parser.add_argument("--settle-probes", type=int, default=0,
                        help="Extra Idle snapshots before Walk (0 keeps the old sequence)")
    parser.add_argument("--settle-interval", type=float, default=1.0,
                        help="Seconds between Idle settle snapshots")
    parser.add_argument("--perf-probes", action="store_true",
                        help="Also submit 'perf json' with each snapshot; sampled, not per-frame dt")
    parser.add_argument("--placement-probes", action="store_true",
                        help="Run the common-actor placement sequence through normal Console "
                             "`mesh` commands (off by default; replaces the Walk sequence)")
    parser.add_argument("--trace", action="store_true",
                        help="Inject one normal Console `hair trace` after startup; "
                             "off by default so existing captures are unchanged")
    parser.add_argument("--trace-frames", type=int, default=120,
                        help="Frames recorded by --trace (default 120)")
    parser.add_argument("--trace-stride", type=int, default=1024,
                        help="Guide selection stride for --trace, max 256 guides (default 1024)")
    args = parser.parse_args()
    if (any(rate < 0 or rate > 1000 for rate in args.rates) or
            not math.isfinite(args.duration) or args.duration < 0 or args.startup_delay < 3 or
            args.pause_probes < 0 or args.settle_probes < 0 or
            not math.isfinite(args.pause_probe_interval) or args.pause_probe_interval <= 0 or
            not math.isfinite(args.settle_interval) or args.settle_interval <= 0 or
            (args.trace and (args.trace_frames <= 0 or args.trace_stride <= 0))):
        parser.error("Rates must be 0..1000 (0=uncapped), duration nonnegative, "
                     "startup-delay at least 3, probe counts nonnegative and intervals positive")
    if args.startup_only and (args.pause_probes or args.settle_probes):
        parser.error("--startup-only cannot be combined with pause/settle probes")
    if args.startup_only and args.placement_probes:
        parser.error("--startup-only cannot be combined with --placement-probes")
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output.exists() or " " in str(output):
        parser.error("Choose a fresh output directory without spaces (Console path tokenization)")
    executable = (root / args.build_directory / "bin/main.exe").resolve()
    if not executable.is_file():
        parser.error(f"Missing executable: {executable}")
    config_path = root / "config/config.json"
    resource = (root / json.loads(config_path.read_bytes())["resourcePath"]).resolve()
    inputs = [config_path, resource / SCENE, resource / HAIR]
    original = {str(path): digest(path) for path in inputs}
    started = time.monotonic()
    with runtime_lock(root):
        output.mkdir(parents=True)
        report = {
            "schemaVersion": SCHEMA_VERSION,
            "tool": {
                "name": "capture_hair_physics.py", "version": TOOL_VERSION,
                "python": platform.python_version(),
            },
            "startedAt": now_iso(),
            "captureArguments": {
                "buildDirectory": str(args.build_directory),
                "rates": args.rates,
                "durationSeconds": args.duration,
                "requireReset": args.require_reset,
                "startupDelayFrames": args.startup_delay,
                "startupOnly": args.startup_only,
                "pauseProbes": args.pause_probes,
                "pauseProbeIntervalSeconds": args.pause_probe_interval,
                "settleProbes": args.settle_probes,
                "settleIntervalSeconds": args.settle_interval,
                "perfProbes": args.perf_probes,
                "placementProbes": args.placement_probes,
                "trace": args.trace,
                "traceFrames": args.trace_frames if args.trace else 0,
                "traceStride": args.trace_stride,
            },
            "completed": False, "executable": str(executable),
            "exeSha256": digest(executable), "inputSha256": original,
            "hairDescriptor": json.loads((resource / HAIR).read_bytes()),
            # 静态矩阵覆盖表随报告保存；placement 三项按本次是否启用探针改写。
            "scenarioCoverage": scenario_coverage(args.placement_probes),
            "runs": [], "physicsNumericalValidation": False,
            "visualReviewRequired": True,
            "completionMeaning": "Capture/log acquisition only; inspect actor visibility and hair",
        }
        try:
            for index, rate in enumerate(args.rates):
                directory = f"{index:02d}-{'uncapped' if rate == 0 else str(rate) + 'hz'}"
                report["runs"].append(run_rate(
                    root, executable, output / directory, rate,
                    args.duration, args.require_reset, args.startup_delay, args.startup_only,
                    args.pause_probes, args.pause_probe_interval,
                    args.settle_probes, args.settle_interval, args.perf_probes,
                    args.placement_probes,
                    args.trace_frames if args.trace else 0, args.trace_stride))
            report["completed"] = True
        finally:
            apply_trace_results(report)
            report["inputsUnchanged"] = all(
                digest(Path(path)) == value for path, value in original.items())
            report["finishedAt"] = now_iso()
            report["elapsedSeconds"] = time.monotonic() - started
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "completed": report["completed"], "inputsUnchanged": report["inputsUnchanged"],
        "output": str(output), "rates": args.rates,
        "traceFrames": sum(
            run.get("trace", {}).get("frames", 0)
            for run in report["runs"] if isinstance(run.get("trace"), dict)),
        "unsupportedScenarios": [
            entry["id"] for entry in report["scenarioCoverage"]
            if entry["status"] == "unsupported"],
    }, indent=2))


if __name__ == "__main__":
    main()
