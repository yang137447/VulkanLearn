import argparse
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    first = np.eye(10)
    last = np.eye(10)
    last[6, 6] = 0
    last[9, 9] = 0
    last[0, 6] = -1
    last[6, 0] = 1
    last[0, 9] = 1
    last[9, 0] = -1
    lower = np.zeros((10, 10))
    upper = np.zeros((10, 10))
    lower[9, 0] = 1
    upper[0, 9] = -1
    matrix = np.block([[first, upper], [lower, last]])
    schur = last - lower @ np.linalg.solve(first, upper)
    right = np.arange(1, 21, dtype=np.float64) / 20
    direction = np.linalg.solve(matrix, right)
    relative = np.linalg.norm(matrix @ direction - right) / np.linalg.norm(right)
    assert np.linalg.matrix_rank(last) == 9
    assert np.linalg.matrix_rank(schur) == 10
    assert np.linalg.matrix_rank(matrix) == 20
    assert relative < 1e-14
    report = {
        "scope": "incoming/right-owned edge plus parallel local contact; two-node algebraic actual-layout counterexample",
        "localLastBlockRank": int(np.linalg.matrix_rank(last)),
        "globalRank": int(np.linalg.matrix_rank(matrix)),
        "forwardSchurRank": int(np.linalg.matrix_rank(schur)),
        "globalRelativeResidual": float(relative),
        "lastBlockSmallestSingularValue": float(np.linalg.svd(last, compute_uv=False)[-1]),
        "globalConditionNumber": float(np.linalg.cond(matrix)),
        "conclusion": "global system is solvable; eliminating last node first requires a singular local pivot. Naive CR or PCR local inversion is invalid. This ordering permits forward Thomas, but does not establish rank safety for every contact/edge topology.",
        "physicalShift": 0,
        "matrix": matrix.tolist(),
        "right": right.tolist(),
        "direction": direction.tolist()
    }
    options.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in (
        "localLastBlockRank", "globalRank", "forwardSchurRank",
        "globalRelativeResidual", "conclusion")}, indent=2))


if __name__ == "__main__":
    main()
