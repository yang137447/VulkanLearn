"""Rebaseline VL's existing Fig.11/13 assets with one shared display calibration."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
CASES = {
    "satin": ("satin", "SC_paper_case_ashikhmin_premoze_shirley_2000_fig11_satin"),
    "velvet": ("velvet", "SC_Cloth_Ashikhmin2000_Fig11Velvet"),
    "slanted_black": ("slantedVelvet", "SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_black"),
    "slanted_red": ("slantedVelvet", "SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_red"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/ashikhmin-paper")
    args = parser.parse_args()
    records = {}
    for name in args.cases:
        variant, scene = CASES[name]
        destination = args.output / name
        subprocess.run([
            sys.executable, str(ROOT / "tool/cloth-intake/render_offline_showcase.py"),
            "--variants", variant, "--scene", f"Maps/{scene}/{scene}.json",
            "--samples", str(args.samples), "--bounces", "0",
            "--display-exposure", "1", "--output", str(destination),
        ], cwd=ROOT, check=True)
        records[name] = json.loads((destination / "results.json").read_text())[0]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "results.json").write_text(json.dumps({
        "cases": records,
        "calibration": {
            "authoredDirectionalLux": 5000,
            "cameraEv100": 9,
            "manualDisplayExposure": 1,
            "resolvedExposure": 2 ** -9 / 1.2,
            "mapping": "ACES fitted + sRGB",
            "reason": "Match realtime camera exposure and default ACES display.",
            "claim": "Local paper-closure appearance, not recovered paper radiometry or geometry.",
        },
    }, indent=2), encoding="utf-8")
    if "satin" in records and "velvet" in records:
        subprocess.run([
            sys.executable, str(Path(__file__).with_name("verify_ashikhmin_fig11.py")),
            records["satin"]["export"], records["velvet"]["export"],
            "--expected-light-exposure", str(5000 * 2 ** -9 / 1.2),
        ], check=True)
    if "slanted_black" in records and "slanted_red" in records:
        subprocess.run([
            sys.executable, str(Path(__file__).with_name("verify_ashikhmin_fig13.py")),
            records["slanted_black"]["export"], records["slanted_red"]["export"],
            "--expected-exposure", str(2 ** -9 / 1.2),
        ], check=True)


if __name__ == "__main__":
    main()
