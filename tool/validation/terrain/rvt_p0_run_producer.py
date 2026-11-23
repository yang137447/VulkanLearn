"""Measure the frozen production binary, preserving raw queries and PID memory."""

import argparse
import json
from pathlib import Path

from runtime_stress import sampled_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    options = parser.parse_args()
    stage, root, output = options.stage.resolve(), options.resources.resolve(), options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config_path = stage / "config/config.json"
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    config["resourcePath"] = root.as_posix()
    config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    (output / "config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
    binary = options.binary.resolve() if options.binary else stage / "bin/main_editor.exe"
    command = [str(binary), "--terrain-vt-cook",
               "--exit-after-frames", "2", "--no-dev-ui", "--no-validation",
               "--worker-thread-count", "1", "--present-mode", "immediate"]
    process, text = sampled_process(command, stage, output / "runtime.log", 900, 500)
    samples = []
    for line in text.splitlines():
        marker = "[BAKE_GPU_RAW] "
        if marker in line:
            samples.append(json.JSONDecoder().raw_decode(line.split(marker, 1)[1])[0])
    report = {"process": process, "samples": samples,
              "passed": process["exitCode"] == 0 and bool(samples) and
              not any(process["detectedErrors"].values()) and
              "EngineLoop teardown complete" in text,
              "scope": "Actual real-source Cook RecordPage plus GPU readback copy; no claim of runtime atlas copy"}
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "samples": len(samples), "process": process}, indent=2))
    if not report["passed"]:
        raise RuntimeError("P0 producer measurement failed; inspect preserved log")


if __name__ == "__main__":
    main()
