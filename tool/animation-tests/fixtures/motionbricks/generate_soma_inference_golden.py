"""Regenerate soma_inference_golden.json with ONNX Runtime on CPU.

Requires an installed `onnxruntime` Python package and the SOMA FP32 ONNX model.
The model is not checked into this repository. See README.md for provenance.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, type=Path,
        help="mmm_v6_soma_fp32.onnx")
    parser.add_argument("--fixture", type=Path,
        default=Path(__file__).with_name("soma_golden.json"))
    parser.add_argument("--output", type=Path,
        default=Path(__file__).with_name("soma_inference_golden.json"))
    args = parser.parse_args()

    fixture = json.loads(args.fixture.read_text(encoding="utf-8"))
    encoded = fixture["encoded"]

    def tensor(name: str, width: int | None, dtype):
        value = np.asarray(encoded[name], dtype=dtype)
        return value.reshape(1, 8, width) if width else value.reshape(1, 8)

    inputs = {
        "global_root_values": tensor("global_root_values", 5, np.float32),
        "local_root_values": tensor("local_root_values", 4, np.float32),
        "local_poses": tensor("local_poses", 267, np.float32),
        "has_global_root_values": tensor("has_global_root_values", None, np.int32),
        "has_local_root_values": tensor("has_local_root_values", None, np.int32),
        "has_local_poses": tensor("has_local_poses", None, np.int32),
        "allowed_pred_num_tokens": np.asarray(
            encoded["allowed_pred_num_tokens"], dtype=np.int32).reshape(1, 11),
    }

    session = ort.InferenceSession(str(args.model), providers=["CPUExecutionProvider"])
    outputs = session.run(None, inputs)
    prediction = outputs[0]
    token_count = int(outputs[1][0])
    if prediction.shape != (1, 64, 366):
        raise RuntimeError(f"Unexpected prediction shape {prediction.shape}")
    if not 6 <= token_count <= 16:
        raise RuntimeError(f"Unexpected token count {token_count}")
    if not np.isfinite(prediction).all():
        raise RuntimeError("Prediction contains a non-finite value")

    golden = {
        "source": f"ONNX Runtime {ort.__version__} CPUExecutionProvider, {args.model.name}",
        "token_count": token_count,
        "pred_global_motions": prediction[0].astype(np.float32).tolist(),
    }
    args.output.write_text(json.dumps(golden, indent=1), encoding="utf-8")
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes), tokens={token_count}")


if __name__ == "__main__":
    main()
