"""Extract raw P0 timing distributions and the actual WDDM sampling cadence."""

import argparse
from collections import defaultdict
from datetime import datetime
import json
from pathlib import Path
import statistics

from runtime_stress import percentile


def distribution(values):
    return {"count": len(values), "minimum": min(values) if values else None,
            "p50": percentile(values, 50), "p95": percentile(values, 95),
            "p99": percentile(values, 99), "maximumObserved": max(values) if values else None}


def extract(text, marker):
    samples, errors, position = [], [], 0
    decoder = json.JSONDecoder()
    while True:
        found = text.find(marker, position)
        if found < 0:
            break
        start = found + len(marker)
        try:
            sample, length = decoder.raw_decode(text[start:])
            samples.append(sample)
            position = start + length
        except json.JSONDecodeError:
            errors.append({"offset": found, "preview": text[start:start + 100]})
            position = start
    return samples, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    options = parser.parse_args()
    root = options.run.resolve()
    text = (root / "runtime.log").read_text(encoding="utf-8", errors="replace")
    gpu, gpu_errors = extract(text, "[GPU_RAW] ")
    cpu, cpu_errors = extract(text, "[CPU_RAW] ")
    bake, bake_errors = extract(text, "[BAKE_GPU_RAW] ")
    passes = defaultdict(list)
    totals = []
    for frame in gpu:
        valid = [sample for sample in frame["passes"] if isinstance(sample.get("ms"), (int, float))]
        totals.append(sum(sample["ms"] for sample in valid))
        for sample in valid:
            passes[sample["name"]].append(sample["ms"])
    memory = [json.loads(line) for line in (root / "runtime.gpu-memory.jsonl").read_text().splitlines() if line.strip()]
    valid_memory = [row for row in memory if row.get("available")]
    moments = [datetime.fromisoformat(row["utc"]) for row in valid_memory]
    intervals = [(second - first).total_seconds() for first, second in zip(moments, moments[1:])]
    result = {"gpuRawFrames": len(gpu), "cpuRawFrames": len(cpu),
              "gpuInstrumentedPassSumMs": distribution(totals),
              "gpuPassesMs": {name: distribution(values) for name, values in passes.items()},
              "cpuRenderFrameIntervalMs": distribution([row["renderFrameIntervalMs"] for row in cpu]),
              "rawParseErrors": {"gpu": gpu_errors, "cpu": cpu_errors, "bake": bake_errors},
              "gpuSamplesUnavailable": [row for row in memory if not row.get("available")],
              "gpuValidSamples": len(valid_memory),
              "gpuActualSamplingIntervalSeconds": distribution(intervals),
              "gpuMemoryScope": "PID WDDM samples; exact transient GPU high-water is not measured",
              "timingScope": "Raw per-rendered-frame GPU pass intervals, not rolling means; serial pass sum excludes uninstrumented environment work. CPU is unclamped wall interval between render frames, including pacing and World replacement, not CPU work duration.",
              "bakeByModeMip": {}}
    grouped = defaultdict(list)
    for sample in bake:
        grouped[(sample["weightSource"], sample["mip"], sample["heightBlend"])].append(sample)
    for key, values in grouped.items():
        result["bakeByModeMip"][str(key)] = {
            "productionMs": distribution([sample["productionMs"] for sample in values]),
            "readbackCopyMs": distribution([sample["readbackCopyMs"] for sample in values])}
    for name, samples in (("gpu", gpu), ("cpu", cpu), ("bake", bake)):
        (root / (name + "-raw.jsonl")).write_text(
            "".join(json.dumps(sample) + "\n" for sample in samples), encoding="utf-8")
    (root / "raw-analysis.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("gpuRawFrames", "cpuRawFrames",
                      "gpuInstrumentedPassSumMs", "cpuRenderFrameIntervalMs",
                      "gpuActualSamplingIntervalSeconds")}, indent=2))


if __name__ == "__main__":
    main()
