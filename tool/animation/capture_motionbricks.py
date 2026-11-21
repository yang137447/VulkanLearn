import argparse
import json
import math
import os
import shutil
import subprocess
import time
from pathlib import Path


def traces(path):
    result = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "MOTIONBRICKS_TRACE " in line:
            result.append(json.loads(line.split("MOTIONBRICKS_TRACE ", 1)[1]))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("artifacts/motionbricks"))
    parser.add_argument("--stages", nargs="+", default=["idle", "walk", "run", "right", "left", "back", "stop"])
    parser.add_argument("--scene", default="Maps/SC_motionbricks/SC_motionbricks_live_probe.json")
    parser.add_argument("--blueprint", default="Maps/SC_motionbricks/Blueprints/BP_motionbricks_live.json")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    resources = args.resource_root.resolve()
    folder = resources / "Maps/SC_motionbricks/Verification"
    if folder.exists():
        raise RuntimeError(f"Verification workspace already exists: {folder}")
    folder.mkdir()
    source = folder / "MotionBricksProbe.blueprint.cs"
    scene_path = folder / "SC_motionbricks_verification.json"
    summary = []
    try:
        shutil.copyfile(Path(__file__).with_name("motionbricksProbe.blueprint.cs"), source)
        subprocess.run(["cmake", "--build", str(root / "build"), "--target", "vl_gameplay", "-j", "4"],
                       cwd=root, check=True)
        scene = json.loads((resources / args.scene).read_text(encoding="utf-8"))
        blueprint = json.loads((resources / args.blueprint).read_text(encoding="utf-8"))
        player = next(item for item in scene["objects"] if item.get("id") == "player")
        player.pop("blueprintPath")
        for key in ("physics", "components"):
            player[key] = blueprint[key]
        player["class"] = "MotionBricksVerification.Probe"
        for stage in args.stages:
            player["properties"] = {"Stage": stage}
            scene["description"] = "MotionBricks 基础运动自动验证：原生实时生成、重定向、C# 根运动与 Jolt 胶囊，非论文验证。"
            scene_path.write_text(json.dumps(scene), encoding="utf-8")
            log_path = output / f"{stage}.log"
            error_path = output / f"{stage}.err"
            screenshot = output / f"{stage}.bmp"
            screenshot.unlink(missing_ok=True)
            def inject(command):
                subprocess.run(["pwsh", "-NoProfile", "-File", str(root / "tool/validation/console_inject.ps1"),
                                "-InjectProcessId", str(process.pid), "-InjectLine", command],
                               cwd=root, check=True, capture_output=True, timeout=20)
            with log_path.open("w", encoding="utf-8") as log, error_path.open("w", encoding="utf-8") as error:
                process = subprocess.Popen([str(root / "build/bin/main_editor.exe"),
                    "--initial-scene", scene_path.relative_to(resources).as_posix(),
                    "--no-dev-ui", "--console-command-delay", "2", "--console-command", "script play"],
                    cwd=root, env=os.environ, stdout=log, stderr=error)
                try:
                    deadline = time.monotonic() + 120
                    records = []
                    while time.monotonic() < deadline:
                        records = traces(log_path)
                        generated = "[MotionBricks] live provider=CPU" in log_path.read_text(encoding="utf-8", errors="replace")
                        if generated and records and records[-1]["elapsed"] >= 7:
                            break
                        if process.poll() is not None:
                            raise RuntimeError(f"{stage}: engine exited; inspect {error_path}")
                        time.sleep(0.1)
                    if not records or records[-1]["elapsed"] < 7:
                        raise RuntimeError(f"{stage}: probe timed out; inspect {error_path}")
                    inject(f"screenshot {screenshot.as_posix()}")
                    deadline = time.monotonic() + 15
                    while not screenshot.exists() and time.monotonic() < deadline:
                        time.sleep(0.1)
                    if not screenshot.exists():
                        raise RuntimeError(f"{stage}: screenshot missing")
                    stable = [record for record in records if record["elapsed"] >= 5]
                    velocity = [sum(record["velocity"][axis] for record in stable) / len(stable)
                                for axis in range(3)]
                    summary.append({"stage": stage, "velocity": velocity,
                                    "grounded": all(record["grounded"] for record in stable),
                                    "screenshot": str(screenshot)})
                    print(json.dumps(summary[-1]), flush=True)
                    if stage == "walk":
                        inject("script pause")
                        time.sleep(0.5)
                        paused_count = len(traces(log_path))
                        time.sleep(0.5)
                        if len(traces(log_path)) != paused_count:
                            raise RuntimeError("walk: script continued ticking while paused")
                        inject("script play")
                        deadline = time.monotonic() + 10
                        while len(traces(log_path)) <= paused_count and time.monotonic() < deadline:
                            time.sleep(0.1)
                        if len(traces(log_path)) <= paused_count:
                            raise RuntimeError("walk: script did not resume")
                        inject("script stop")
                        inject("script play")
                        deadline = time.monotonic() + 20
                        while time.monotonic() < deadline:
                            replay = traces(log_path)
                            if len(replay) > paused_count + 3 and replay[-1]["elapsed"] < 3:
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError("walk: Stop/Play did not recreate the runtime")
                        summary[-1]["pauseResumeStopReplay"] = True
                    inject("quit")
                    process.wait(timeout=20)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=10)
            errors = error_path.read_text(encoding="utf-8", errors="replace")
            if "DisableBlueprint" in errors or "VUID" in errors:
                raise RuntimeError(f"{stage}: runtime errors; inspect {error_path}")
        (output / "runtime-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        directions = {"walk": (0, -1), "run": (0, -1), "right": (1, 0),
                      "left": (-1, 0), "back": (0, 1)}
        for record in summary:
            stage = record["stage"]
            velocity = record["velocity"]
            speed = math.hypot(velocity[0], velocity[2])
            if not record["grounded"]:
                raise RuntimeError(f"{stage}: character lost ground contact")
            if stage in ("idle", "stop"):
                if speed >= 25:
                    raise RuntimeError(f"{stage}: excessive drift {speed:.1f} cm/s")
            else:
                direction = directions[stage]
                along = velocity[0] * direction[0] + velocity[2] * direction[1]
                if speed < 80 or speed > 600 or along < speed * 0.7:
                    raise RuntimeError(f"{stage}: incorrect velocity {velocity}")
    finally:
        source.unlink(missing_ok=True)
        scene_path.unlink(missing_ok=True)
        folder.rmdir()
        subprocess.run(["cmake", "--build", str(root / "build"), "--target", "vl_gameplay", "-j", "4"],
                       cwd=root, check=True)


if __name__ == "__main__":
    main()
