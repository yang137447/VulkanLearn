import argparse
import hashlib
import json
from pathlib import Path

from analyze_repeat_tail_numeric import coverage, encode, evaluate, rounded_byte


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sampling-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    measured = json.loads(options.sampling_report.read_text())
    report = {
        "scope": "CPU analysis of already measured GPU floats; no GPU submission",
        "sourceReport": str(options.sampling_report),
        "sourceReportSha256": digest(options.sampling_report),
        "referencesPreserved": ["original production sampler textureLod",
                                "same sampler with anisotropy disabled",
                                "same sRGB-view texelFetch plus float manual bilinear",
                                "independent double IEC sRGB decode and continuous bilinear"],
        "points": [],
        "limitations": [
            "Only the measured fixture points, mip0 and albedo were diagnosed here",
            "No whole-page/manual-candidate or complete source-mip GPU comparison",
            "Manual blend reconstruction below is CPU arithmetic over measured GPU samples",
            "No guarantee that hardware textureLod versus manual filtering stays within one output byte",
        ],
    }
    original, disabled = measured["cases"]
    points = {}
    for query in original["queries"]:
        points.setdefault(tuple(query["stored"]), []).append(query)
    for pixel, queries in points.items():
        uv = [((128 + pixel[0] - 4 + .5) / 129) % 1,
              ((pixel[1] - 4 + .5) / 129) % 1]
        weights, _ = coverage(uv)
        hardware = sum(weights[query["globalLayer"]] * query["textureLod"][0] for query in queries)
        fetched = sum(weights[query["globalLayer"]] * query["manualBilinear"][0] for query in queries)
        independent = evaluate(uv)
        hardware_byte = rounded_byte(hardware)
        fetched_byte = rounded_byte(fetched)
        double_byte = independent["byte"]
        report["points"].append({
            "stored": pixel, "domainUvDouble": uv, "layerWeights": weights,
            "originalActualGpuTextureLodLinear": hardware,
            "originalActualGpuTextureLodByte": hardware_byte,
            "measuredFetchManualBlendLinear": fetched,
            "measuredFetchManualBlendEncodedContinuous": encode(fetched) * 255,
            "measuredFetchManualBlendByte": fetched_byte,
            "independentDoubleLinear": independent["linear"],
            "independentDoubleByte": double_byte,
            "manualVsDoubleByteError": abs(fetched_byte - double_byte),
            "manualVsOriginalActualGpuByteError": abs(fetched_byte - hardware_byte),
            "integerOutputsWithinOneOfBoth": list(range(
                max(0, hardware_byte - 1, double_byte - 1),
                min(255, hardware_byte + 1, double_byte + 1) + 1)),
        })
    report["anisotropyToggleChangesAnyMeasuredFloat"] = any(
        first["textureLod"] != second["textureLod"]
        for first, second in zip(original["queries"], disabled["queries"]))
    report["status"] = "comparison-conflict-preserved"
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
