import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spirv", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    result = subprocess.run(["spirv-dis", "--raw-id", str(options.spirv)],
                            capture_output=True, text=True, encoding="utf-8", check=True)
    instructions = []
    for line in result.stdout.splitlines():
        match = re.search(r"\b(Op\w+)\b", line)
        if match:
            instructions.append(match.group(1))
    counts = Counter(instructions)
    report = {
        "gpuExecuted": False, "spirvBytes": options.spirv.stat().st_size,
        "instructionCount": len(instructions), "opcodes": dict(sorted(counts.items())),
        "samplingInstructions": sum(value for key, value in counts.items() if key.startswith("OpImageSample")),
        "textureFetchInstructions": counts["OpImageFetch"],
        "loopHeaders": counts["OpLoopMerge"], "switches": counts["OpSwitch"],
        "functions": counts["OpFunction"], "functionCalls": counts["OpFunctionCall"],
        "limitations": [
            "SPIR-V is not NVIDIA machine code; static instruction/loop counts do not measure registers or occupancy",
            "No pipeline-executable statistics or Nsight GPU profiler run in this CPU-only analysis",
        ],
    }
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2))
    print(json.dumps({key: value for key, value in report.items() if key != "opcodes"}, indent=2))


if __name__ == "__main__":
    main()
