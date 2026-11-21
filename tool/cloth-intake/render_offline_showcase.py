"""Render the generated showcase through VL and export its Direct/Indirect AOVs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT.parent / "VukanLearnResources"
INJECT = ROOT / "tool/validation/console_inject.ps1"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build-cloth-csharp/bin/main_game.exe")
    parser.add_argument("--variants", nargs="+", choices=["satin", "velvet", "slantedVelvet"],
                        default=["satin", "velvet", "slantedVelvet"])
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--bounces", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--scene", help="Existing VL scene override; requires one variant label.")
    parser.add_argument("--display-exposure", type=float, default=1.0)
    parser.add_argument("--capture-realtime", action="store_true")
    parser.add_argument("--realtime-only", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/cloth-offline")
    args = parser.parse_args()
    if args.scene and len(args.variants) != 1:
        parser.error("--scene requires exactly one --variants label")
    if args.display_exposure <= 0:
        parser.error("--display-exposure must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for variant in args.variants:
        if args.capture_realtime or args.realtime_only:
            screenshot = (args.output / f"{variant}_realtime.bmp").resolve()
            with (args.output / f"{variant}_realtime.log").open("wb") as output:
                subprocess.run([
                    str(args.exe), "--no-dev-ui", "--worker-thread-count", "1",
                    "--initial-scene",
                    f"Maps/SC_cloth_showcase/SC_cloth_showcase_{variant}_direct.json",
                    "--exit-after-frames", "150", "--frame-limit", "60",
                    "--console-command", f"screenshot {screenshot}",
                    "--console-command-delay", "100",
                ], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=120)
        if args.realtime_only:
            continue
        log = args.output / f"{variant}.log"
        error = args.output / f"{variant}.err.log"
        command = [
            str(args.exe), "--offline", "--no-dev-ui", "--worker-thread-count", "1",
            "--frame-limit", "30", "--initial-scene",
            args.scene or f"Maps/SC_cloth_showcase/SC_cloth_showcase_{variant}_offline.json",
            "--console-command", f"offline samples {args.samples}",
            "--console-command", f"offline bounces {args.bounces}",
            "--console-command", f"offline exposure {args.display_exposure}",
            "--console-command-delay", "2",
        ]
        with log.open("wb") as output, error.open("wb") as errors:
            process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=errors,
                                       creationflags=subprocess.CREATE_NEW_CONSOLE)

            def send(line: str) -> None:
                subprocess.run(
                    ["pwsh", "-NoProfile", "-File", str(INJECT),
                     "-InjectProcessId", str(process.pid), "-InjectLine", line],
                    check=True, capture_output=True, text=True, timeout=20,
                )

            try:
                started = time.monotonic()
                exported = None
                last_export = started
                while time.monotonic() - started < args.timeout:
                    text = log.read_text(encoding="utf-8", errors="replace")
                    if process.poll() is not None:
                        raise RuntimeError(f"{variant}: VL exited {process.returncode}; see {log}")
                    if "Offline output:" in text:
                        match = re.findall(r"Offline output: ([^\r\n]+)", text)[-1]
                        exported = Path(match)
                        if not exported.is_absolute():
                            exported = ROOT / exported
                        manifest = json.loads((exported / "manifest.json").read_text(encoding="utf-8"))
                        if manifest["samples"] >= args.samples:
                            subprocess.run(
                                [sys.executable, str(ROOT / "tool/validation/offline/verify_vl_export.py"),
                                 str(exported)], check=True, capture_output=True, text=True,
                            )
                            break
                    if "Debug console ready" in text and time.monotonic() - last_export > 8:
                        send("offline export")
                        last_export = time.monotonic()
                    if "Scene.LoadFailed" in text or "Offline.MaterialUnsupported" in text:
                        raise RuntimeError(f"{variant}: offline preflight failed; see {log}")
                    time.sleep(1)
                else:
                    raise TimeoutError(f"{variant}: no completed export within {args.timeout}s; see {log}")
                results.append({"variant": variant, "export": str(exported), "samples": manifest["samples"],
                                "bounces": args.bounces, "seconds": round(time.monotonic() - started, 2)})
                print(json.dumps(results[-1], ensure_ascii=False), flush=True)
            finally:
                if process.poll() is None:
                    send("quit")
                    process.wait(timeout=30)
    if results:
        (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
