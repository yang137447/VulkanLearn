import argparse
import json
from pathlib import Path

import numpy as np


def sample(path):
    metadata = json.loads(Path(str(path) + ".json").read_text(encoding="utf-8"))
    positions = np.fromfile(path, dtype="<f4").reshape(40334, 32, 4)[..., :3].astype(np.float64)
    state = np.fromfile(str(path) + ".rod-state.bin", dtype="<f4").reshape(2000, 412, 4).astype(np.float64)
    points = state[:, :160].reshape(2000, 32, 5, 4)
    segments = state[:, 160:408].reshape(2000, 31, 8, 4)
    speeds2 = (points[:, :, 1, :3] ** 2).sum(axis=-1)
    angular2 = segments[:, :, 1, :3] ** 2
    linear_energy = .5 * (points[:, :, 3, 0] * speeds2).sum()
    angular_energy = .5 * (segments[:, :, 5, :3] * angular2).sum()
    return metadata, positions, {
        "acceptedMin": metadata["currentBufferAcceptedSecondsMin"],
        "acceptedMax": metadata["currentBufferAcceptedSecondsMax"],
        "gpuStatusMax": metadata["currentBufferGpuStatusMax"],
        "failedGuides": metadata["currentBufferFailedGuides"],
        "componentResidualMax": metadata["currentBufferComponentResidualMax"],
        "pointSpeedMaxMPerSecond": float(np.sqrt(speeds2.max())),
        "bodyOmegaMaxRadPerSecond": float(np.sqrt(angular2.sum(axis=-1).max())),
        "selectedGuideTranslationalKineticJ": float(linear_energy),
        "selectedGuideSectionRotationalKineticJ": float(angular_energy),
        "equalPointCentroidCm": positions.reshape(-1, 3).mean(axis=0).tolist()
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--first", type=Path, required=True)
    parser.add_argument("--second", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    first_meta, first_positions, first = sample(options.first)
    second_meta, second_positions, second = sample(options.second)
    assert first_meta["worldGeneration"] == second_meta["worldGeneration"]
    assert first_meta["simulationGeneration"] == second_meta["simulationGeneration"]
    assert first_meta["simulationSourceGuideIds"] == second_meta["simulationSourceGuideIds"]
    elapsed = second["acceptedMin"] - first["acceptedMin"]
    assert elapsed >= 2
    displacement_cm = np.linalg.norm(second_positions - first_positions, axis=-1)
    centroid_delta_cm = np.asarray(second["equalPointCentroidCm"]) - first["equalPointCentroidCm"]
    report = {
        "scope": "same-run two fence-safe actual40334 snapshots with all2000 distinct simulation states",
        "physicalWindowSeconds": elapsed,
        "first": first, "second": second,
        "maximumPointDriftCm": float(displacement_cm.max()),
        "rmsPointDriftCm": float(np.sqrt(np.mean(displacement_cm ** 2))),
        "equalPointCentroidDriftCm": float(np.linalg.norm(centroid_delta_cm)),
        "maximumPointDriftRateMPerSecond": float(displacement_cm.max() * .01 / elapsed),
        "limitations": [
            "Kinetic energy sums only the physical simulation guides; follow strands have no independent inertia.",
            "Two endpoints establish net drift, not a bound on all intermediate oscillations.",
            "Equal-point centroid is geometric, not mass-weighted.",
            "Contact, implicit-air and axial-inequality model qualifications remain separate."
        ],
        "productionQualityPass": False
    }
    options.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
