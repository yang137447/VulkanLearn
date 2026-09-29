"""External Windows stress driver for the VL offline viewport.

Run from any directory:
  py -3 tool/validation/offline/stress_vl_integration.py --workers 1 2
  py -3 tool/validation/offline/stress_vl_integration.py --scenarios standard render-exit

No runtime test flag or config edit is involved. Runs are serial because they
share shader/spv and the resource root. Logs and timing JSON stay under artifacts/.
"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parents[3]
INJECT = REPO / "tool" / "validation" / "console_inject.ps1"
VERIFY = Path(__file__).with_name("verify_vl_export.py")
SCENE = "Maps/SC_offline_shared/SC_offline_shared.json"
RENDER_EXIT_SAMPLES = 1000000
EXPORT_LINE = re.compile(r"\[Diagnostics\]\[Info\] Offline output: ([^\r\n]+)")
RESIZE_LINE = re.compile(r"\[Diagnostics\]\[Info\] Window resized to (\d+)x(\d+)")
SESSION_WARNING = re.compile(
    r"\[Diagnostics\]\[Warning\] (?:Offline session action is unavailable|"
    r"Offline session command requires|Offline mode:|Invalid offline setting)"
)


def export_path_since(log_text, start, resource_root):
    """Only a new success diagnostic counts; never scan old resource directories."""
    matches = EXPORT_LINE.findall(log_text[start:])
    if not matches:
        return None
    path = Path(matches[-1].strip()).resolve()
    root = (resource_root / "Generated" / "OfflineRenderer").resolve()
    if path.parent != root:
        raise ValueError(f"export outside configured resource root: {path}")
    return path


def check_manifest(manifest, scene, dimensions, seed, bounces):
    expected = {
        "worldGeneration": 1,
        "samples": 1,
        "targetSamples": 1,
        "complete": True,
    }
    if Path(manifest.get("scene", "")).resolve() != scene.resolve():
        raise ValueError(f"manifest scene does not match {scene}")
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise ValueError(f"manifest {key}: expected {value!r}, got {manifest.get(key)!r}")
    for key, value in {
        "width": dimensions[0],
        "height": dimensions[1],
        "seed": seed,
        "bounces": bounces,
    }.items():
        if manifest.get("integrator", {}).get(key) != value:
            raise ValueError(f"manifest integrator.{key}: expected {value}")
    if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("inputDigest", "")):
        raise ValueError("manifest has no full input digest")
    for name in ("combined.pfm", "direct.pfm", "indirect.pfm", "combined.ppm"):
        if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("outputs", {}).get(name, "")):
            raise ValueError(f"manifest has no {name} digest")


def check_partial_manifest(manifest, scene, dimensions):
    if manifest.get("targetSamples") != RENDER_EXIT_SAMPLES:
        return False  # 旧任务结果可能在参数命令被消费前发布，继续等待新任务。
    if Path(manifest.get("scene", "")).resolve() != scene.resolve():
        raise ValueError(f"partial manifest scene does not match {scene}")
    if manifest.get("worldGeneration") != 1:
        raise ValueError("partial manifest has wrong World generation")
    integrator = manifest.get("integrator", {})
    if integrator.get("width") != dimensions[0] or \
            integrator.get("height") != dimensions[1]:
        raise ValueError("partial manifest has wrong dimensions")
    if integrator.get("seed") != 101:
        raise ValueError("partial manifest has wrong seed")
    samples = manifest.get("samples")
    if not isinstance(samples, int) or isinstance(samples, bool) or \
            not 0 < samples < RENDER_EXIT_SAMPLES or manifest.get("complete") is not False:
        raise ValueError("high-spp result is not an incomplete published film")
    if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("inputDigest", "")):
        raise ValueError("partial manifest has no full input digest")
    for name in ("combined.pfm", "direct.pfm", "indirect.pfm", "combined.ppm"):
        if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("outputs", {}).get(name, "")):
            raise ValueError(f"partial manifest has no {name} digest")
    return True


def teardown_issue(exit_code, log_text, error_text):
    if exit_code != 0:
        return f"main.exe exit code: {exit_code}"
    quit_at = log_text.find("Quit event received")
    teardown_at = log_text.find("EngineLoop teardown complete", quit_at + 1)
    if quit_at < 0 or teardown_at < 0:
        return "missing ordered quit/teardown diagnostics"
    if "Render thread failed" in log_text or "terminate called" in log_text:
        return "renderer failure in main.log"
    if error_text.strip():
        return "main.err.log is not empty"
    return None


def run_succeeded(result):
    return ("error" not in result and "closeError" not in result and
            result.get("closeRequested") is True and result.get("cleanClose") is True)


def read_log(path):
    return path.read_bytes().decode("utf-8", errors="replace") if path.exists() else ""


def wait_for(process, log_path, predicate, description, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = read_log(log_path)
        value = predicate(text)
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError(f"main.exe exited ({process.returncode}) while waiting for {description}")
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {description}; see {log_path}")


def select_renderer_window(windows):
    # SDL owns the titled viewport; IME/helper windows can share its PID.
    matches = [hwnd for hwnd, title in windows if title.startswith("VulkanRenderer")]
    if len(matches) != 1:
        raise RuntimeError(f"expected one VulkanRenderer window, got {windows}")
    return matches[0]


def owned_window(process_id, timeout):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (enum_proc, wintypes.LPARAM)
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.GetWindowTextLengthW.argtypes = (wintypes.HWND,)
    user32.GetWindowTextLengthW.restype = ctypes.c_int
    user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetClientRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
    user32.GetClientRect.restype = wintypes.BOOL
    user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
    user32.GetWindowRect.restype = wintypes.BOOL
    user32.SetWindowPos.argtypes = (
        wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, wintypes.UINT,
    )
    user32.SetWindowPos.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    user32.PostMessageW.restype = wintypes.BOOL
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        windows = []

        @enum_proc
        def collect(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process_id and user32.IsWindowVisible(hwnd):
                title = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(hwnd) + 1)
                user32.GetWindowTextW(hwnd, title, len(title))
                windows.append((hwnd, title.value))
            return True

        if not user32.EnumWindows(collect, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        if any(title.startswith("VulkanRenderer") for _, title in windows):
            return user32, select_renderer_window(windows)
        time.sleep(0.1)
    raise TimeoutError(f"no visible VulkanRenderer window owned by PID {process_id}")


def resize_client(user32, hwnd, width, height):
    client = wintypes.RECT()
    outer = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(client)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not user32.GetWindowRect(hwnd, ctypes.byref(outer)):
        raise ctypes.WinError(ctypes.get_last_error())
    outer_width = outer.right - outer.left + width - (client.right - client.left)
    outer_height = outer.bottom - outer.top + height - (client.bottom - client.top)
    if not user32.SetWindowPos(hwnd, None, 0, 0, outer_width, outer_height, 0x0002 | 0x0004 | 0x0010):
        raise ctypes.WinError(ctypes.get_last_error())


def send_command(process, log_path, line, timeout, events, expected=None, settle=0.15):
    start = len(read_log(log_path))
    started = time.monotonic()
    command = [
        "pwsh", "-NoProfile", "-File", str(INJECT),
        "-InjectProcessId", str(process.pid), "-InjectLine", line,
    ]
    injection = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if injection.returncode:
        raise RuntimeError(f"console injection of {line!r} failed: "
                           f"{injection.stdout} {injection.stderr}")
    # _getch echoes the line before the queued runtime command is executed.
    echo = re.compile(r"(?:^|\n)(?:debug> )?" + re.escape(line) + r"\r?\n")
    wait_for(process, log_path, lambda text: echo.search(text[start:]), f"echo of {line}", timeout)
    if expected:
        wait_for(process, log_path, lambda text: expected in text[start:],
                 f"{line}: {expected}", timeout)
    time.sleep(settle)
    warning = SESSION_WARNING.search(read_log(log_path)[start:])
    if warning:
        raise RuntimeError(f"{line}: {warning.group(0)}")
    events.append({"action": line, "seconds": round(time.monotonic() - started, 3)})
    return start


def run_render_exit(process, log_path, user32, window, args, resource_root, events):
    resize_start = len(read_log(log_path))
    resize_client(user32, window, args.width, args.height)
    match = wait_for(process, log_path,
                     lambda text: RESIZE_LINE.search(text[resize_start:]),
                     "window resize before high-spp render", args.timeout)
    dimensions = (int(match.group(1)), int(match.group(2)))
    events.append({"action": "resize", "requested": [args.width, args.height],
                   "reported": dimensions})
    send_command(process, log_path, "offline seed 101", args.timeout, events)
    send_command(process, log_path, f"offline samples {RENDER_EXIT_SAMPLES}",
                 args.timeout, events)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        export_start = send_command(process, log_path, "offline export",
                                    args.timeout, events, settle=0.02)
        path = export_path_since(read_log(log_path), export_start, resource_root)
        if not path:
            try:
                path = wait_for(
                    process, log_path,
                    lambda text: export_path_since(text, export_start, resource_root),
                    "partial high-spp export", 1,
                )
            except TimeoutError:
                pass
        if path:
            manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
            if check_partial_manifest(manifest, resource_root / args.scene, dimensions):
                # 部分 film 已由积分 worker 发布；保持高目标，立即交给 finally 关闭窗口。
                return path, manifest
        time.sleep(1)
    raise TimeoutError("no incomplete high-spp film was published within retry budget")


def run_one(worker, args, run_root, resource_root, scenario="standard"):
    run_dir = run_root / (f"worker-{worker}" if scenario == "standard"
                          else f"{scenario}-worker-{worker}")
    run_dir.mkdir()
    log_path = run_dir / "main.log"
    error_path = run_dir / "main.err.log"
    events = []
    result = {"worker": worker, "scenario": scenario, "events": events, "status": "FAIL"}
    process = None
    window = None
    started = time.monotonic()
    try:
        with log_path.open("wb") as out, error_path.open("wb") as err:
            process = subprocess.Popen(
                [str(args.exe), "--offline", "--initial-scene", args.scene,
                 "--worker-thread-count", str(worker), "--no-dev-ui"],
                cwd=REPO, stdout=out, stderr=err,
                creationflags=subprocess.CREATE_NEW_CONSOLE,
            )
            result["pid"] = process.pid
            wait_for(process, log_path, lambda text: "Debug console ready" in text,
                     "console ready", args.timeout)
            wait_for(process, log_path, lambda text: "World/graph transaction committed: generation=1" in text,
                     "initial World commit", args.timeout)
            result["startupSeconds"] = round(time.monotonic() - started, 3)
            user32, window = owned_window(process.pid, args.timeout)

            if scenario == "render-exit":
                path, manifest = run_render_exit(
                    process, log_path, user32, window, args, resource_root, events)
                result["partialExport"] = str(path)
                result["publishedSamples"] = manifest["samples"]
                result["targetSamples"] = manifest["targetSamples"]
            else:
                # Keep target high until after pause/resume/stop; each injection is a
                # separate process so AttachConsole cannot corrupt the driver's handles.
                for line in ("offline samples 48", "offline bounces 1", "offline seed 3",
                             "offline samples 64", "offline bounces 2", "offline seed 9"):
                    send_command(process, log_path, line, args.timeout, events, settle=0.02)
                for line in ("offline pause", "offline resume", "offline stop", "offline restart"):
                    send_command(process, log_path, line, args.timeout, events)
                send_command(process, log_path, "offline off", args.timeout, events, "Realtime mode")
                send_command(process, log_path, "offline on", args.timeout, events, "Offline mode")

                # Switch AOV without resetting accumulation, then make a new final task.
                for line in ("offline aov direct", "offline aov indirect", "offline aov combined",
                             "offline seed 17", "offline bounces 2", "offline samples 1"):
                    send_command(process, log_path, line, args.timeout, events, settle=0.02)
                resize_start = len(read_log(log_path))
                resize_client(user32, window, args.width, args.height)
                match = wait_for(
                    process, log_path,
                    lambda text: RESIZE_LINE.search(text[resize_start:]),
                    "window resize", args.timeout,
                )
                dimensions = (int(match.group(1)), int(match.group(2)))
                events.append({"action": "resize", "requested": [args.width, args.height],
                               "reported": dimensions, "seconds": round(time.monotonic() - started, 3)})

                export_started = time.monotonic()
                # A new success line, not the presence of an older output folder, is the
                # publication marker. Retry while a full sample is still being prepared.
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    export_start = send_command(process, log_path, "offline export",
                                                args.timeout, events, settle=0.02)
                    path = export_path_since(read_log(log_path), export_start, resource_root)
                    if not path:
                        try:
                            path = wait_for(
                                process, log_path,
                                lambda text: export_path_since(text, export_start, resource_root),
                                "new offline export", 1,
                            )
                        except TimeoutError:
                            pass
                    if path:
                        break
                    time.sleep(1)
                if not path:
                    raise TimeoutError("no newly published offline export within retry budget")
                manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
                check_manifest(manifest, resource_root / args.scene, dimensions, seed=17, bounces=2)
                verification = subprocess.run(
                    [sys.executable, str(VERIFY), str(path)], cwd=REPO,
                    capture_output=True, text=True, timeout=args.timeout,
                )
                (run_dir / "verify.log").write_text(
                    verification.stdout + verification.stderr, encoding="utf-8")
                if verification.returncode:
                    raise RuntimeError(f"verify_vl_export.py failed: {verification.stderr}")
                result["export"] = str(path)
                result["exportSeconds"] = round(time.monotonic() - export_started, 3)
                result["manifest"] = {
                    "inputDigest": manifest["inputDigest"],
                    "taskGeneration": manifest["taskGeneration"],
                    "samples": manifest["samples"],
                    "integrator": manifest["integrator"],
                }
                result["verification"] = verification.stdout.strip()
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if process is not None:
            close_started = time.monotonic()
            if process.poll() is None:
                try:
                    if window is None:
                        user32, window = owned_window(process.pid, 3)
                    if not user32.PostMessageW(window, 0x0010, 0, 0):  # WM_CLOSE
                        raise ctypes.WinError(ctypes.get_last_error())
                    result["closeRequested"] = True
                    process.wait(timeout=args.timeout)
                except Exception as exc:
                    result["closeError"] = str(exc)
                    # Never force-kill a renderer that may own shared resources.
            else:
                result["closeError"] = "main.exe exited before WM_CLOSE"
            result["exitCode"] = process.poll()
            result["closeSeconds"] = round(time.monotonic() - close_started, 3)
            text = read_log(log_path)
            issue = teardown_issue(result["exitCode"], text, read_log(error_path))
            result["cleanClose"] = issue is None
            if issue and "closeError" not in result:
                result["closeError"] = issue
        result["totalSeconds"] = round(time.monotonic() - started, 3)
        if run_succeeded(result):
            result["status"] = "PASS"
        (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workers", nargs="+", type=int, choices=(1, 2), default=(1, 2))
    parser.add_argument("--scenarios", nargs="+", choices=("standard", "render-exit"),
                        default=("standard",), help="serial scenarios for each worker mode")
    parser.add_argument("--scene", default=SCENE)
    parser.add_argument("--exe", type=Path, default=REPO / "build" / "bin" / "main.exe")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--timeout", type=int, default=120, help="per-operation timeout in seconds")
    parser.add_argument("--output-root", type=Path,
                        default=REPO / "artifacts" / "validation" / "offline" / "vl-stress")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this driver requires Windows console and window APIs")
    if args.width < 64 or args.height < 64 or args.timeout < 3:
        parser.error("width/height must be at least 64; timeout must be at least 3")
    if len(set(args.workers)) != len(args.workers):
        parser.error("each worker mode may be requested only once")
    if len(set(args.scenarios)) != len(args.scenarios):
        parser.error("each scenario may be requested only once")
    args.exe = args.exe.resolve()
    if not args.exe.is_file():
        parser.error(f"executable not found: {args.exe}")
    config = json.loads((REPO / "config" / "config.json").read_text(encoding="utf-8"))
    resource_root = Path(config["resourcePath"]).resolve()
    if not (resource_root / args.scene).is_file():
        parser.error(f"scene not found under resourcePath: {args.scene}")
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    run_root = output_root / (datetime.now().strftime("%Y%m%d-%H%M%S-%f") + f"-{os.getpid()}")
    run_root.mkdir()
    results = []
    for scenario in args.scenarios:
        for worker in args.workers:
            result = run_one(worker, args, run_root, resource_root, scenario)
            results.append(result)
            detail = (result.get("export", result.get("partialExport", ""))
                      if result["status"] == "PASS" else
                      result.get("error", result.get("closeError", "")))
            print(f"{scenario} worker {worker}: {result['status']} in {result['totalSeconds']}s; "
                  f"{detail}",
                  flush=True)
            if result["status"] != "PASS":
                break  # 退出/join 失败时不能再启动共享 shader/spv 的进程。
        if results[-1]["status"] != "PASS":
            break
    (run_root / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"timings and logs: {run_root}")
    return 0 if len(results) == len(args.workers) * len(args.scenarios) and \
        all(r["status"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
