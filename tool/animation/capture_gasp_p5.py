"""Capture real C#/Jolt/Motion Matching stages through production API and Console."""

import argparse
import ctypes
import html
import json
import math
import os
import struct
import subprocess
import sys
import time
from pathlib import Path


MARKER = "GASP_P5_TRACE "
STAGES = ("idle", "start", "walk", "stop", "reverse", "run", "jump", "land")
OPTIONAL_STAGES = ("blocked", "slope", "stairs")
EXPECTED_CLIPS = {
    "idle": ["M_Neutral_Stand_Idle_Loop"],
    "start": ["M_Neutral_Walk_Start_F_Lfoot"],
    "walk": ["M_Neutral_Walk_Loop_F"],
    "stop": ["M_Neutral_Walk_Stop_F_Lfoot"],
    "reverse": ["M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R"],
    "run": ["M_Neutral_Run_Loop_F", "M_Neutral_Sprint_Loop_F"],
    "jump": ["M_Neutral_Jump_F_Start_Stand_Lfoot"],
    "land": ["M_Neutral_Jump_F_Land_Stand_Light_Lfoot"],
    "blocked": ["M_Neutral_Stand_Idle_Loop"],
    "slope": ["M_Neutral_Stand_Idle_Loop"],
    "stairs": ["M_Neutral_Walk_Loop_F"],
}
SOURCE = r"""using System;
using System.Numerics;
using System.Text.Json;
using VulkanLearn;
namespace GaspP5Capture;
// 独立确定性输入夹具；只提交业务允许片段，查询、选帧、混合和时钟仍由生产原生路径拥有。
public sealed class CharacterFixture : Blueprint
{
    [Expose(InstanceEditable = true)] public string Stage { get; set; } = "walk";
    [Expose(InstanceEditable = true)] public bool MatchSpeed { get; set; } = false;
    [Expose(InstanceEditable = true)] public bool FootPlacement { get; set; } = false;
    [Expose(InstanceEditable = true)] public bool Inertialization { get; set; } = false;
    [Expose(InstanceEditable = true)] public bool RootOffset { get; set; } = false;
    private AnimationPlayback animation = null!;
    private float elapsed, lastTrace = -1, jumpTime = -1, activeElapsed = -1, loopStableSince = -1,
        blockedStableSince = -1, slopeStableSince = -1;
    private Vector3 desiredVelocity;
    private long lastSearch = -1;
    private int inputFrames;
    private string lastSelection = "";
    private bool jumped, airborne, landed, ready;
    private int stairTreadsCrossed;
    private string phase = "warmup";
    private string[] allowed = Idle;
    private static readonly AssetId Database = new("Maps/SC_gasp/MotionMatching/DB_gasp.json");
    private static readonly string[] Idle = {
        "M_Neutral_Stand_Idle_Loop", "M_Neutral_Walk_Stop_F_Lfoot"
    };
    private static readonly string[] Grounded = {
        "M_Neutral_Stand_Idle_Loop", "M_Neutral_Walk_Loop_F",
        "M_Neutral_Walk_Start_F_Lfoot", "M_Neutral_Walk_Stop_F_Lfoot",
        "M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R"
    };
    private static readonly string[] Running = {
        "M_Neutral_Stand_Idle_Loop", "M_Neutral_Run_Loop_F", "M_Neutral_Sprint_Loop_F",
        "M_Neutral_Walk_Start_F_Lfoot", "M_Neutral_Walk_Stop_F_Lfoot",
        "M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R"
    };
    protected override void OnCreate()
    {
        animation = GetComponent<MeshComponent>("visual").Animation;
        if (Character == null) throw new InvalidOperationException("Character body is required");
    }
    protected override void BeginPlay() => GetComponent<CameraComponent>("camera").Activate();
    protected override void Tick(float dt)
    {
        ++inputFrames;
        elapsed += dt;
        if (activeElapsed >= 0) activeElapsed += dt;
        float active = activeElapsed;
        var velocity = Vector3.Zero;
        phase = active < 0 ? "warmup" : Stage;
        if (active >= 0)
        {
            if (Stage is "start" or "walk" or "blocked" or "stairs") velocity = new(0, 0, -200);
            if (Stage == "run") velocity = new(0, 0, -450);
            if (Stage is "stop" or "reverse")
            {
                if (active < 1) { phase = "prewalk"; velocity = new(0, 0, -200); }
                else if (Stage == "reverse") velocity = new(0, 0, 200);
            }
            if (Stage is "jump" or "land")
            {
                if (!jumped && Character!.IsGrounded)
                {
                    jumped = true; jumpTime = elapsed; Character.Jump();
                }
            }
        }
        desiredVelocity = velocity;
        Character!.Move(velocity);
        if (velocity.LengthSquared() > 0)
            Transform.Rotation = new(0, MathF.Atan2(-velocity.X, -velocity.Z) * 180 / MathF.PI, 0);
        // 只跟随实际角色平移，抵消 Actor 朝向，避免失败等待或暂停动画后角色走出截图。
        // 向画面左侧留出角色位置，右侧仍可审阅真实 Motion 调试表。
        var camera = GetComponent<CameraComponent>("camera");
        float yaw = Transform.Rotation.Y;
        var undoYaw = Quaternion.CreateFromAxisAngle(Vector3.UnitY, -yaw * MathF.PI / 180);
        camera.Transform.Position = Vector3.Transform(new Vector3(788, 510, 634), undoYaw);
        camera.Transform.Rotation = new(-28, 40 - yaw, 0);
    }
    protected override void LateTick(float dt)
    {
        var body = Character!;
        if (jumped && !body.IsGrounded) airborne = true;
        bool landing = airborne && body.IsGrounded && !landed;
        if (landing && animation.State.Clip == "M_Neutral_Jump_F_Land_Stand_Light_Lfoot" &&
            animation.State.Completed)
        {
            landed = true; landing = false;
        }
        bool actionLock = false;
        if (!body.IsGrounded)
        {
            allowed = [body.LinearVelocity.Y > 0 ?
                "M_Neutral_Jump_F_Start_Stand_Lfoot" : "M_Neutral_Jump_Loop_Fall"];
            actionLock = body.LinearVelocity.Y > 0;
        }
        else if (landing && (animation.State.Clip != "M_Neutral_Jump_F_Land_Stand_Light_Lfoot" ||
                 !animation.State.Completed))
        {
            allowed = ["M_Neutral_Jump_F_Land_Stand_Light_Lfoot"];
            actionLock = true;
        }
        // 与正式高层状态机一致：实际停止（含碰墙）只比较停止与待机；
        // 实际移动仍保留完整六/七成员，不能用动作名称缩小起步/转身候选。
        else if (new Vector2(body.LinearVelocity.X, body.LinearVelocity.Z).Length() < 2)
            allowed = Idle;
        else allowed = Stage == "run" && activeElapsed >= 0 ? Running : Grounded;
        animation.Match(Database, allowed, actionLocked: actionLock, matchSpeed: MatchSpeed,
            footPlacement: FootPlacement, inertialization: Inertialization, rootOffset: RootOffset);
        var motion = animation.Motion;
        var playback = animation.State;
        var feet = animation.FootPlacement;
        // Debug 首次资源/环境准备可产生大 dt；先等待真实历史/query 就绪，再开始输入矩阵。
        // 这样不把启动停顿跳过的起步阶段伪装为“稳定输入未触发起步”。
        if (activeElapsed < 0 && motion.QueryReady && body.IsGrounded) activeElapsed = 0;
        bool correctSelection = Array.IndexOf(allowed, motion.SelectedClip) >= 0;
        bool expectedSelection = motion.SelectedClip == playback.Clip && (Stage switch {
            "idle" => motion.SelectedClip == "M_Neutral_Stand_Idle_Loop",
            "start" => motion.SelectedClip == "M_Neutral_Walk_Start_F_Lfoot",
            "walk" => motion.SelectedClip == "M_Neutral_Walk_Loop_F",
            "stop" => motion.SelectedClip == "M_Neutral_Walk_Stop_F_Lfoot",
            "reverse" => motion.SelectedClip is "M_Neutral_Stand_Turn_180_L" or "M_Neutral_Stand_Turn_180_R",
            "run" => motion.SelectedClip is "M_Neutral_Run_Loop_F" or "M_Neutral_Sprint_Loop_F",
            "jump" => motion.SelectedClip == "M_Neutral_Jump_F_Start_Stand_Lfoot",
            "land" => motion.SelectedClip == "M_Neutral_Jump_F_Land_Stand_Light_Lfoot",
            "blocked" => motion.SelectedClip == "M_Neutral_Stand_Idle_Loop",
            "slope" => motion.SelectedClip == "M_Neutral_Stand_Idle_Loop",
            "stairs" => motion.SelectedClip == "M_Neutral_Walk_Loop_F",
            _ => false
        });
        float active = activeElapsed;
        // Run 的开关两组共享同一稳态门槛，避免把过渡帧与稳定帧拿来对照。
        bool requireStableLoop = Stage is "walk" or "run";
        if (requireStableLoop && expectedSelection && motion.QueryReady)
        {
            if (loopStableSince < 0) loopStableSince = active;
        }
        else loopStableSince = -1;
        float stableLoopSeconds = loopStableSince >= 0 ? active - loopStableSince : 0;
        // 仅由真实 Jolt 反馈和本夹具已提交意图推导；不冒充未跨 CLR 发布的原生 query 标志。
        bool physicallyBlocked = body.IsGrounded &&
            new Vector2(desiredVelocity.X, desiredVelocity.Z).LengthSquared() >= 4 &&
            new Vector2(body.LinearVelocity.X, body.LinearVelocity.Z).LengthSquared() < 4;
        if (Stage == "blocked" && physicallyBlocked && expectedSelection && motion.QueryReady)
        {
            if (blockedStableSince < 0) blockedStableSince = active;
        }
        else blockedStableSince = -1;
        float stableBlockedIdleSeconds = blockedStableSince >= 0 ? active - blockedStableSince : 0;
        // 坡面开关两组均静止四秒并稳定接地一秒；求解误差仍由独立模块测量验收。
        if (Stage == "slope" && body.IsGrounded && expectedSelection && motion.QueryReady)
        {
            if (slopeStableSince < 0) slopeStableSince = active;
        }
        else slopeStableSince = -1;
        float stableSlopeSeconds = slopeStableSince >= 0 ? active - slopeStableSince : 0;
        // 六段80cm踏面与模块夹具同源；连续向前通过中心带，不传送也不删除边缘帧。
        if (Stage == "stairs")
            for (int tread = 0; tread < 6; ++tread)
                if (Transform.Position.Z <= -180 - 80 * tread && Transform.Position.Z >= -220 - 80 * tread)
                    stairTreadsCrossed |= 1 << tread;
        bool due = Stage switch {
            "idle" => active >= 0.2f,
            "start" => active >= 0.04f,
            "walk" => active >= 4,
            "stop" or "reverse" => active >= 1.04f,
            "run" => active >= 0.6f,
            "jump" => airborne && !body.IsGrounded && body.LinearVelocity.Y > 0 &&
                      elapsed >= jumpTime + 0.04f,
            "land" => landing,
            "blocked" => active >= 4,
            "slope" => active >= 4,
            "stairs" => Transform.Position.Z <= -680,
            _ => false
        };
        bool stagePassed = due && motion.QueryReady && correctSelection && expectedSelection &&
                           motion.SearchCount > 0 && (!requireStableLoop || stableLoopSeconds >= 0.75f) &&
                           (Stage != "blocked" || stableBlockedIdleSeconds >= 1) &&
                           (Stage != "slope" || stableSlopeSeconds >= 1) &&
                           (Stage != "stairs" || (stairTreadsCrossed == 63 && body.IsGrounded));
        // 选不到预期类别也采集真实失败证据；不得缩小候选集合来强行通过。
        float deadline = requireStableLoop || Stage is "blocked" or "slope" or "stairs" ? 6.0f : Stage is "stop" or "reverse" ? 3.0f :
                         Stage is "jump" or "land" ? 4.0f : 2.5f;
        bool captureNow = !ready && (stagePassed || active >= deadline);
        if (captureNow) ready = true;
        if (elapsed - lastTrace >= 0.08f || captureNow || motion.SearchCount != lastSearch ||
            motion.SelectedClip != lastSelection)
        {
            lastTrace = elapsed;
            lastSearch = motion.SearchCount;
            lastSelection = motion.SelectedClip;
            var position = Transform.Position;
            var velocity = body.LinearVelocity;
            Log.Info("GASP_P5_TRACE " + JsonSerializer.Serialize(new {
                stage = Stage, phase, elapsed, activeElapsed, inputFrames, deltaSeconds = dt,
                captureReady = captureNow, due,
                expectedSelection, stagePassed, stableLoopSeconds, stableBlockedIdleSeconds, stableSlopeSeconds,
                physicallyBlocked, stairTreadsCrossed,
                desiredVelocity = new[] { desiredVelocity.X, desiredVelocity.Y, desiredVelocity.Z },
                grounded = body.IsGrounded,
                locomotion = !body.IsGrounded ? (body.LinearVelocity.Y > 0 ? "jump" : "fall") :
                    landing ? "land" : "grounded",
                position = new[] { position.X, position.Y, position.Z },
                velocity = new[] { velocity.X, velocity.Y, velocity.Z },
                allowed, actionLock, queryReady = motion.QueryReady,
                databaseIdentity = motion.DatabaseIdentity, decision = motion.Decision,
                searchCount = motion.SearchCount, selectedClip = motion.SelectedClip,
                selectedTime = motion.SelectedTime, clip = playback.Clip,
                time = playback.Time, duration = playback.Duration, completed = playback.Completed,
                matchSpeed = MatchSpeed, rate = playback.Rate, speedMatching = motion.SpeedMatching,
                footPlacementEnabled = FootPlacement, footPlacement = feet,
                inertializationEnabled = Inertialization, rootOffsetEnabled = RootOffset,
                rootOffset = animation.RootOffset
            }, new JsonSerializerOptions { IncludeFields = true }));
        }
        Hud.SetText("capture", $"P5 {Stage} | {phase} | {elapsed:F2}s\n" +
            $"{motion.SelectedClip} {motion.SelectedTime:F2}s\n" +
            $"{motion.Decision} | searches {motion.SearchCount}\n" +
            $"Rate {playback.Rate:F2} | residual {motion.SpeedMatching.ResidualSpeed:F0} cm/s\n" +
            $"Feet {feet.Reason} | pelvis {feet.PelvisOffset:F1} cm");
    }
}
public sealed class Floor : Blueprint { }
"""


def inject_console(process_id, line):
    """Run only in a short-lived child: AttachConsole changes process standard handles."""
    from ctypes import wintypes

    class KeyEvent(ctypes.Structure):
        _fields_ = [("down", wintypes.BOOL), ("repeat", wintypes.WORD),
                    ("key", wintypes.WORD), ("scan", wintypes.WORD),
                    ("character", wintypes.WCHAR), ("control", wintypes.DWORD)]

    class Event(ctypes.Union):
        _fields_ = [("key", KeyEvent), ("padding", ctypes.c_byte * 16)]

    class InputRecord(ctypes.Structure):
        _fields_ = [("kind", wintypes.WORD), ("event", Event)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.WriteConsoleInputW.argtypes = [wintypes.HANDLE, ctypes.POINTER(InputRecord),
                                         wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.FreeConsole()
    if not kernel.AttachConsole(process_id):
        raise ctypes.WinError(ctypes.get_last_error())
    handle = kernel.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
    if not handle or handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        text = line + "\r"
        records = (InputRecord * (len(text) * 2))()
        for index, character in enumerate(text):
            for offset, down in ((0, True), (1, False)):
                record = records[index * 2 + offset]
                record.kind = 1
                record.event.key = KeyEvent(down, 1, 0, 0, character, 0)
        written = wintypes.DWORD()
        if not kernel.WriteConsoleInputW(handle, records, len(records), ctypes.byref(written)):
            raise ctypes.WinError(ctypes.get_last_error())
        if written.value != len(records):
            raise RuntimeError("Incomplete console command injection")
    finally:
        kernel.CloseHandle(handle)
        kernel.FreeConsole()


def trace_records(text):
    decoder = json.JSONDecoder()
    records = []
    for line in text.splitlines():
        if MARKER in line:
            try:
                value, _ = decoder.raw_decode(line.split(MARKER, 1)[1])
                records.append(value)
            except json.JSONDecodeError:
                pass  # The logging thread may still be completing this final line.
    return records


def complete_bmp(path):
    if not path.is_file() or path.stat().st_size < 54:
        return False
    with path.open("rb") as stream:
        header = stream.read(54)
    if header[:2] != b"BM":
        return False
    width, height = struct.unpack_from("<ii", header, 18)
    bits = struct.unpack_from("<H", header, 28)[0]
    offset = struct.unpack_from("<I", header, 10)[0]
    return width > 0 and height != 0 and bits == 24 and path.stat().st_size == (
        offset + ((width * 3 + 3) & ~3) * abs(height))


def selection_class(clip):
    return next((stage for stage, clips in EXPECTED_CLIPS.items() if clip in clips), "other")


def expected_behavior(record, stage):
    return bool(record.get("stagePassed") and record.get("queryReady") and record.get("searchCount", 0) > 0 and
                record.get("databaseIdentity") and record.get("selectedClip") in EXPECTED_CLIPS[stage] and
                record.get("selectedClip") == record.get("clip") and
                record.get("selectedClip") in record.get("allowed", []))


def capture_case(root, executable, output, scene, stage, threads, size, timeout, pause_animation):
    name = f"{stage}-t{threads}-{size[0]}x{size[1]}"
    log_path, error_path = output / f"{name}.log", output / f"{name}.err"
    shot = output / f"{name}.bmp"
    arguments = [str(executable), "--initial-scene", scene, "--worker-thread-count", str(threads),
                 "--dev-ui", "--console-command-delay", "1", "--console-command", "motiondebug 1"]
    started = time.monotonic()
    requested = False
    trigger = None
    failure = ""
    completed = False
    with log_path.open("w", encoding="utf-8") as log, error_path.open("w", encoding="utf-8") as error:
        process = subprocess.Popen(arguments, cwd=root, stdout=log, stderr=error)
        try:
            while time.monotonic() - started < timeout:
                text = log_path.read_text(encoding="utf-8", errors="replace")
                traces = trace_records(text)
                if not requested:
                    trigger = next((record for record in traces if record["captureReady"]), None)
                    if not trigger and traces and time.monotonic() - started >= max(1, timeout - 20):
                        trigger = dict(traces[-1])
                        trigger["fallbackReason"] = "native-query-or-stage-never-ready"
                    if trigger:
                        command = f"screenshot {shot.as_posix()}"
                        if pause_animation:
                            command = "animation pause\r" + command
                        subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                        "--inject-process", str(process.pid),
                                        "--inject-line", command],
                                       check=True, timeout=20, capture_output=True)
                        requested = True
                if requested and complete_bmp(shot) and "Screenshot saved:" in text:
                    completed = True
                    break
                if process.poll() is not None:
                    raise RuntimeError(f"{name}: engine exited {process.returncode}; inspect logs")
                time.sleep(0.025)
            else:
                raise TimeoutError(f"{name}: no ready native feedback/screenshot; inspect logs")
        except Exception as error:
            failure = f"{type(error).__name__}: {error}"
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=20)
            traces = trace_records(log_path.read_text(encoding="utf-8", errors="replace"))
            (output / f"{name}.trace.json").write_text(json.dumps(traces, indent=2), encoding="utf-8")
    traces = trace_records(log_path.read_text(encoding="utf-8", errors="replace"))
    errors = error_path.read_text(encoding="utf-8", errors="replace")
    if errors.strip():
        failure += f"\nNonempty stderr; inspect {error_path}"
    if not traces or not trigger or not trigger["databaseIdentity"]:
        failure += "\nMissing production Motion Matching feedback"
    if completed:
        from PIL import Image, ImageStat
        with Image.open(shot) as image:
            if image.size != size:
                failure += f"\nExpected {size}, captured {image.size}"
                completed = False
            if max(ImageStat.Stat(image.convert("RGB")).stddev) < 1:
                failure += "\nBlank screenshot"
                completed = False
    passed_records = [record for record in traces if expected_behavior(record, stage)]
    text = log_path.read_text(encoding="utf-8", errors="replace")
    preceding = trace_records(text.split("Screenshot saved:", 1)[0])
    captured = preceding[-1] if preceding else None
    captured_expected = bool(captured and captured.get("selectedClip") in EXPECTED_CLIPS[stage] and
                             captured.get("selectedClip") == captured.get("clip"))
    result = {"name": name, "stage": stage, "threads": threads, "size": size,
              "expectedClips": EXPECTED_CLIPS[stage],
              "actualSelectionClasses": sorted({selection_class(record.get("selectedClip", ""))
                                                for record in traces if record.get("due")}),
              "captureCompleted": completed, "behaviorPassed": bool(passed_records),
              "feedbackBeforeCaptureMatchesExpected": captured_expected,
              "behaviorEvidence": passed_records[0] if passed_records else None,
              "failure": failure.strip(), "trigger": trigger, "feedbackBeforeCapture": captured,
              "lastNativeFeedback": traces[-1] if traces else None, "traceRecords": len(traces),
              "recordedMaxDeltaSeconds": max((record.get("deltaSeconds", 0) for record in traces), default=0),
              "recordedHistoryGapFrames": sum(record.get("deltaSeconds", 0) > 0.5 for record in traces),
              "captureDelayGameSeconds": captured["elapsed"] - trigger["elapsed"] if captured and trigger else None,
              "screenshotBehaviorPairing": "not exact: managed feedback and RT capture have different frame boundaries",
              "animationPausedBeforeCapture": pause_animation,
              "closure": "owned process terminated and waited; not graceful teardown evidence",
              "exitCode": process.returncode}
    result["passed"] = completed and bool(passed_records) and not failure
    print(f"{name}: captured={completed}, behavior={bool(passed_records)}, "
          f"last-feedback={captured.get('selectedClip', '') if captured else 'missing'}", flush=True)
    return result


def make_stair_mesh(resources, fixture_root):
    """Visible unit box; every tread uses the same dimensions as its Jolt box."""
    vertices = [(-1, -1, -1), (-1, -1, 1), (-1, 1, -1), (-1, 1, 1),
                (1, -1, -1), (1, -1, 1), (1, 1, -1), (1, 1, 1)]
    faces = [((1, 3, 4, 2), (-1, 0, 0)), ((5, 6, 8, 7), (1, 0, 0)),
             ((1, 2, 6, 5), (0, -1, 0)), ((3, 7, 8, 4), (0, 1, 0)),
             ((1, 5, 7, 3), (0, 0, -1)), ((2, 4, 8, 6), (0, 0, 1))]
    lines = ["o stair", "usemtl ground"]
    lines += ["v " + " ".join(map(str, vertex)) for vertex in vertices]
    lines += ["vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"]
    lines += ["vn " + " ".join(map(str, normal)) for _, normal in faces]
    for normal_index, (face, _) in enumerate(faces, 1):
        for triangle in ((0, 1, 2), (0, 2, 3)):
            lines.append("f " + " ".join(f"{face[index]}/{index + 1}/{normal_index}" for index in triangle))
    model = fixture_root / "stairs.obj"
    model.write_text("\n".join(lines) + "\n", encoding="ascii")
    descriptor = fixture_root / "SM_stairs.json"
    descriptor.write_text(json.dumps({
        "name": "P6 analytic stair box", "type": "mesh",
        "modelDataPath": model.relative_to(resources).as_posix(),
        "materialSlots": [{"name": "ground", "materialInstancePath": "Maps/SC_gasp/Materials/MI_ground.json"}]
    }, indent=2), encoding="utf-8")
    return descriptor.relative_to(resources).as_posix()


def make_scene(resources, fixture_root, stage, match_speed=False, foot_placement=False,
               inertialization=False, root_offset=False):
    scene = json.loads((resources / "Maps/SC_gasp/SC_gasp.json").read_text(encoding="utf-8"))
    scene["name"] = f"GASP P5 {stage}"
    scene["description"] = (
        "GASP P5 独立确定性输入验收夹具，经正式 C#/Jolt/原生 Motion Matching；"
        "非论文场景，不代表人工键鼠或 UE 动作等价。")
    scene["objects"] = [item for item in scene["objects"] if item["name"] != "mannequin"]
    scene["objects"] += [
        {"type": "blueprintInstance", "id": "p5-character", "name": "P5 input fixture",
         "class": "GaspP5Capture.CharacterFixture", "position": [0, 90, 0], "rotation": [0, 0, 0],
         "scale": [1, 1, 1], "properties": {"Stage": stage, "MatchSpeed": match_speed,
                                         "FootPlacement": foot_placement, "Inertialization": inertialization,
                                         "RootOffset": root_offset},
         "physics": {"motion": "character", "shape": "capsule", "radius": 30, "halfHeight": 60},
         "components": [{"id": "visual", "type": "mesh", "position": [0, -90, 0],
                         "rotation": [0, 180, 0], "modelPath": "Maps/SC_gasp/Meshes/SM_mannequin.json"},
                        {"id": "camera", "type": "camera", "position": [788, 510, 634],
                         "rotation": [-28, 40, 0], "fov": 48, "near_clip": 1, "far_clip": 10000}]},
        {"type": "blueprintInstance", "id": "p5-floor", "name": "P5 collision floor",
         "class": "GaspP5Capture.Floor", "position": [0, -50, 0], "rotation": [0, 0, 0],
         "scale": [1, 1, 1], "properties": {}, "components": [],
         "physics": {"shape": "box", "halfExtents": [10000, 50, 10000]}}]
    if stage == "blocked":
        scene["objects"].append({
            "type": "blueprintInstance", "id": "p5-wall", "name": "P5 blocking wall",
            "class": "GaspP5Capture.Floor", "position": [0, 150, -250],
            "rotation": [0, 0, 0], "scale": [1, 1, 1], "properties": {},
            "physics": {"shape": "box", "halfExtents": [300, 150, 20]},
            "components": [{"id": "visual", "type": "mesh", "position": [0, 0, 20],
                            "rotation": [90, 0, 0], "scale": [0.5, 1, 0.25],
                            "modelPath": "Maps/SC_gasp/Meshes/SM_ground.json"}]})
    if stage == "slope":
        # 18° 横坡使双脚处于不同高度；碰撞盒上表面与可见地面使用同一平面。
        slope_degrees = 18
        radians = math.radians(slope_degrees)
        floor = next(item for item in scene["objects"] if item.get("id") == "p5-floor")
        floor["rotation"] = [0, 0, slope_degrees]
        floor["position"] = [50 * math.sin(radians), -50 * math.cos(radians), 0]
        floor["physics"]["friction"] = 1
    if stage == "stairs":
        model_path = make_stair_mesh(resources, fixture_root)
        for index, height in enumerate((10, 20, 30, 20, 10, 0)):
            if height == 0:
                continue  # The sixth tread is the authored floor, also counted by the input fixture.
            scene["objects"].append({
                "type": "blueprintInstance", "id": f"p6-tread-{index}", "name": f"P6 tread {index}",
                "class": "GaspP5Capture.Floor", "position": [0, height / 2, -200 - 80 * index],
                "rotation": [0, 0, 0], "scale": [1, 1, 1], "properties": {},
                "physics": {"shape": "box", "halfExtents": [150, height / 2, 40]},
                "components": [{"id": "visual", "type": "mesh", "scale": [150, height / 2, 40],
                                "modelPath": model_path}]})
    for item in scene["objects"]:
        if item["type"] == "mesh" and item["name"] == "ground":
            # 四秒以上的稳态动作会离开 P2 小审阅台；仅扩大独立夹具地面。
            item["scale"] = [10, 1, 10]
            if stage == "slope":
                item["position"] = [0, 0, 0]
                item["rotation"] = [0, 0, 18]
        if item["type"] == "camera":
            item["position"], item["look_at"] = [650, 600, 750], [0, 80, -200]
            item["fov"] = 48
            item["exposure"]["ev100"] = 3
        if item["type"] == "directionalLight":
            item["intensity"] = 10
    path = fixture_root / f"SC_{stage}.json"
    path.write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")
    return path.relative_to(resources).as_posix()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=Path("build-vscode/development"))
    parser.add_argument("--dotnet", type=Path, default=Path("dotnet"))
    parser.add_argument("--threads", default="1,2")
    parser.add_argument("--sizes", default="1280x720,1920x1080,900x700")
    parser.add_argument("--stages", default=",".join(STAGES))
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--pause-animation-before-capture", action="store_true")
    parser.add_argument("--match-speed", action="store_true")
    parser.add_argument("--foot-placement", action="store_true")
    parser.add_argument("--inertialization", action="store_true")
    parser.add_argument("--root-offset", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Production console injection requires Windows")
    from PIL import Image  # Fail before launching an engine if image validation is unavailable.
    root = Path(__file__).resolve().parents[2]
    resources, output, build = args.resources.resolve(), args.output.resolve(), args.build.resolve()
    fixtures = resources / "Generated/Validation" / output.name
    if output.exists() or fixtures.exists():
        parser.error("Choose new output and Generated/Validation directories")
    if not (build / "bin/main.exe").is_file():
        parser.error("Build the C# enabled executable first")
    threads = [int(value) for value in args.threads.split(",")]
    sizes = [tuple(map(int, value.split("x"))) for value in args.sizes.split(",")]
    stages = args.stages.split(",")
    if any(value not in (1, 2) for value in threads) or any(
            stage not in STAGES + OPTIONAL_STAGES for stage in stages):
        parser.error("Use threads 1/2 and the documented stage names")
    if not sizes or any(len(value) != 2 or min(value) < 640 for value in sizes):
        parser.error("Sizes must be WIDTHxHEIGHT, each dimension at least 640")
    output.mkdir(parents=True)
    fixtures.mkdir(parents=True)
    runtime = build / "bin/managed"
    project = output / "Capture.csproj"
    project.write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework>'
        '<Nullable>enable</Nullable><AssemblyName>GaspP5Capture</AssemblyName></PropertyGroup>'
        f'<ItemGroup><Reference Include="VulkanLearn.Runtime"><HintPath>{html.escape(str(runtime / "VulkanLearn.Runtime.dll"))}'
        '</HintPath><Private>false</Private></Reference></ItemGroup></Project>', encoding="utf-8")
    (output / "Capture.cs").write_text(SOURCE, encoding="utf-8")
    dotnet = str(args.dotnet.resolve()) if args.dotnet.is_file() else str(args.dotnet)
    result = subprocess.run([dotnet, "build", str(project), "-c", "Release", "-o",
                             str(output / "managed"), "--nologo"], capture_output=True, text=True,
                            encoding="utf-8")
    (output / "managed-build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    result.check_returncode()
    scenes = {stage: make_scene(resources, fixtures, stage, args.match_speed, args.foot_placement,
                               args.inertialization, args.root_offset)
              for stage in stages}
    config_path = root / "config/config.json"
    original = config_path.read_bytes()
    config = json.loads(original)
    config["resourcePath"] = resources.as_posix()
    config["scripts"] = {"enabled": True, "autoPlay": True, "runtimeDirectory": str(runtime),
                         "assembly": str(output / "managed/GaspP5Capture.dll")}
    report = {"source": "isolated fixed-time input, production C#/Jolt/Motion Matching",
              "candidateCosts": "native Motion debug overlay; managed trace contains finite summary only",
              "determinismScope": "repeatable input schedule; rendered frame times and selections may differ",
              "matchSpeed": args.match_speed,
              "footPlacement": args.foot_placement,
              "inertialization": args.inertialization, "rootOffset": args.root_offset,
              "stairsReference": {"heightsCm": [10, 20, 30, 20, 10, 0], "depthCm": 80, "widthCm": 300,
                                  "startZ": -160, "input": "continuous forward 200cm/s",
                                  "qualityScope": "visual crossing only; independent source-window/mesh diagnostics live in ScriptAnimationTest"}
                                 if "stairs" in stages else None,
              "slopeReference": {"angleDegrees": 18, "point": [0, 0, 0],
                                 "normal": [-math.sin(math.radians(18)), math.cos(math.radians(18)), 0],
                                 "soleScope": "bind ball reference point; not heel or mesh sole ground truth"}
                                if "slope" in stages else None,
              "completed": False, "cases": []}
    try:
        for index, size in enumerate(sizes):
            config["windowSize"] = size
            config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
            # 首尺寸覆盖完整动作矩阵，其余尺寸只核对同一 Walk 调试表布局。
            for stage in stages if index == 0 else (["walk"] if "walk" in stages else stages[:1]):
                for count in threads:
                    report["cases"].append(capture_case(root, build / "bin/main.exe", output,
                                                       scenes[stage], stage, count, size, args.timeout,
                                                       args.pause_animation_before_capture))
        report["completed"] = True
        report["behaviorPassed"] = all(case["behaviorPassed"] for case in report["cases"])
        report["capturesCompleted"] = all(case["captureCompleted"] for case in report["cases"])
        report["passed"] = all(case["passed"] for case in report["cases"])
    finally:
        config_path.write_bytes(original)
        report["configRestored"] = config_path.read_bytes() == original
        (output / "capture.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"report": str(output / "capture.json"), "cases": len(report["cases"]),
                      "capturesCompleted": report.get("capturesCompleted", False),
                      "behaviorPassed": report.get("behaviorPassed", False),
                      "passed": report.get("passed", False), "configRestored": report["configRestored"],
                      "failedCases": [case["name"] for case in report["cases"] if not case["passed"]]},
                     indent=2))
    return 0 if report.get("passed", False) else 1


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--inject-process" and sys.argv[3] == "--inject-line":
        inject_console(int(sys.argv[2]), sys.argv[4])
    else:
        raise SystemExit(main())
