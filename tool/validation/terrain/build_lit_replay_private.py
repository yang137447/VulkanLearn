import argparse
import json
from pathlib import Path
import shutil
import subprocess

from compile_frozen_cpp import compile_frozen, digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = root / "tool/validation/terrain/native-capture"
    snapshot = output / "private"
    snapshot.mkdir()
    for path in tools.iterdir():
        if path.is_file():
            shutil.copyfile(path, snapshot / path.name)
    diagnostics = snapshot / "render/diagnostics"
    diagnostics.mkdir(parents=True)
    for suffix in ("h", "cpp"):
        shutil.copyfile(tools / ("terrainLitCaptureHooks."+suffix+".in"),
                        diagnostics / ("terrainLitCaptureHooks."+suffix))
    previous = json.loads((root / "artifacts/terrain-rvt-production/r10-actual-draw-private-r3/adapter-final-identity-compile.json").read_text())
    flags = ["-I"+str(snapshot), "-I"+str(root / "source"),
             "-I"+str(root / "source/pipeline")]
    flags += [flag for flag in previous["flags"]
              if not flag.startswith("-I")]
    sources = [snapshot / "terrainLitTarget.cpp", snapshot / "terrainLitReplaySession.cpp",
               snapshot / "terrainLitCaptureAdapter.cpp",
               snapshot / "terrainLitCase.cpp",
               diagnostics / "terrainLitCaptureHooks.cpp"]
    records = []
    for source in sources:
        records.append(compile_frozen(previous["compiler"], flags, source,
                       output / (source.stem+".o"), root))
    command = [previous["compiler"], "-r", *[item["object"] for item in records],
               "-o", str(output / "lit-components.o")]
    linked = subprocess.run(command, capture_output=True, text=True)
    (output / "link.log").write_text(linked.stdout+linked.stderr)
    linked.check_returncode()
    report = {"gpuExecuted": False, "scope": "Private native component compile and relocatable link only",
              "baseHookSha256": digest(root / "source/render/diagnostics/terrainCaptureHooks.h"),
              "compilationClosures": records,
              "componentSha256": digest(output / "lit-components.o")}
    (output / "build.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"compiled": len(records), "componentSha256": report["componentSha256"]}))


if __name__ == "__main__":
    main()
