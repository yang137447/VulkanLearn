"""External P2 captures using production launch/console commands, never CTest."""
import argparse
import json
import struct
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image


def capture(executable, root, output, name, scene, commands, threads=1, delay=30, editor=False):
    shot = output / f"{name}.bmp"
    arguments = [str(executable), "--initial-scene", scene, "--worker-thread-count", str(threads),
                 "--dev-ui" if editor else "--no-dev-ui", "--console-command-delay", str(delay)]
    for command in [*commands, f"screenshot {shot.as_posix()}"]:
        arguments += ["--console-command", command]
    with (output / f"{name}.log").open("w", encoding="utf-8") as log, \
            (output / f"{name}.err").open("w", encoding="utf-8") as error:
        process = subprocess.Popen(arguments, cwd=root, stdout=log, stderr=error)
        try:
            deadline = time.monotonic() + 150
            while time.monotonic() < deadline:
                if shot.is_file() and shot.stat().st_size > 54:
                    with shot.open("rb") as stream:
                        header = stream.read(54)
                    width, height = struct.unpack_from("<ii", header, 18)
                    expected = struct.unpack_from("<I", header, 10)[0] + ((width * 3 + 3) & ~3) * abs(height)
                    if shot.stat().st_size == expected:
                        # 文件长度先就绪，原生写出完成日志再作为提交确认。
                        if "Screenshot saved:" in (output / f"{name}.log").read_text(encoding="utf-8"):
                            break
                if process.poll() is not None:
                    raise RuntimeError(f"{name}: engine exited {process.returncode}; inspect logs")
                time.sleep(0.25)
            else:
                raise TimeoutError(f"{name}: screenshot timeout")
        finally:
            # 原生窗口缺少 quit 命令；这里只结束自己创建的进程，不声称正常 teardown 验收。
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=30)
    errors = (output / f"{name}.err").read_text(encoding="utf-8")
    if errors.strip():
        raise RuntimeError(f"{name}: nonempty stderr; inspect logs")
    print(f"Captured {name}", flush=True)
    return np.asarray(Image.open(shot).convert("RGB"), dtype=np.int16)


def comparison(first, second):
    # 左侧 runtime scene-name HUD 与场景身份有关，不属于蒙皮对照。ROI 含角色及完整阴影。
    delta = np.abs(first[100:600, 500:1150] - second[100:600, 500:1150])
    return {"meanByteError": float(delta.mean()), "maxByteError": int(delta.max()),
            "pixelsOver4": int((delta.max(axis=2) > 4).sum()), "pixels": int(delta.shape[0] * delta.shape[1])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cpu-scene", default="Generated/Validation/GaspCpu025/SC_cpu.json")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output.exists():
        parser.error("Choose a new output directory")
    output.mkdir(parents=True)
    executable = root / "build/bin/main.exe"
    scene = "Maps/SC_gasp/SC_gasp.json"
    images = {}
    cases = [
        ("gpu025t1", scene, ["animation clip 1", "animation seek 0.25"], 1, 30, False),
        ("gpu075t1", scene, ["animation clip 1", "animation seek 0.75"], 1, 30, False),
        ("gpu025t2", scene, ["animation clip 1", "animation seek 0.25"], 2, 30, False),
        ("pausedRepeat", scene, ["animation clip 1", "animation seek 0.25"], 1, 60, False),
        ("cpu025", args.cpu_scene, [], 1, 30, False),
        ("normalGpu", scene, ["animation clip 1", "animation seek 0.25", "debugview 3"], 1, 30, False),
        ("normalCpu", args.cpu_scene, ["debugview 3"], 1, 30, False),
        ("velocityPaused", scene, ["animation clip 1", "animation seek 0.25", "debugview 90"], 1, 30, False),
        ("velocityMoving", scene, ["debugview 90"], 2, 30, False),
        ("editor", scene, ["animation clip 1", "animation seek 0.25"], 2, 30, True),
    ]
    for name, path, commands, threads, delay, editor in cases:
        images[name] = capture(executable, root, output, name, path, commands, threads, delay, editor)
    report = {
        "cpuLit": comparison(images["gpu025t1"], images["cpu025"]),
        "cpuNormal": comparison(images["normalGpu"], images["normalCpu"]),
        "singleDual": comparison(images["gpu025t1"], images["gpu025t2"]),
        "pausedRepeat": comparison(images["gpu025t1"], images["pausedRepeat"]),
        "poseChange": comparison(images["gpu025t1"], images["gpu075t1"]),
        "processClosure": "Owned capture processes terminated; not graceful teardown evidence",
    }
    # 先固定容差：CPU/GPU 累加次序可有边缘舍入，>4 字节像素不得超过 ROI 的 0.1%。
    for key in ["cpuLit", "cpuNormal", "singleDual", "pausedRepeat"]:
        result = report[key]
        result["passed"] = result["pixelsOver4"] <= result["pixels"] * 0.001
    report["poseChange"]["passed"] = report["poseChange"]["pixelsOver4"] > 1000
    paused = images["velocityPaused"][100:600, 500:1150]
    moving = images["velocityMoving"][100:600, 500:1150]
    # 天空没有 deferred 速度，保留附件 clear 值；仅检查实际 GBuffer 几何。
    # 固定 normal 对照中几何的最大通道 >100，HDRI 背景均低于此值。
    geometry = images["normalCpu"][100:600, 500:1150].max(axis=2) > 100
    zero = paused[-1, -1]  # 固定静态地面，显示端编码后的零速度。
    paused_delta = np.abs(paused - zero).max(axis=2)[geometry]
    # moving 是 Idle，无根位移，固定地面与角色覆盖范围都在同一几何掩码内。
    moving_delta = np.abs(moving - zero).max(axis=2)[geometry]
    report["velocity"] = {"pausedNonzeroPixels": int((paused_delta > 2).sum()),
                          "movingNonzeroPixels": int((moving_delta > 2).sum())}
    report["velocity"]["passed"] = (report["velocity"]["pausedNonzeroPixels"] == 0 and
                                     report["velocity"]["movingNonzeroPixels"] > 100)
    report["passed"] = all(value.get("passed", True) for value in report.values() if isinstance(value, dict))
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
