"""Bind completed native raw TES files to the numerical reader without altering either input."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from rvt_visual_oracle import decode_tes, digest, read, require

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--native-metadata", type=Path, required=True)
    parser.add_argument("--native-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = read(args.native_build)
    executable = Path(build["executable"])
    require(digest(executable) == build["executableSha256"], "Native verifier changed")
    require(not args.output.exists(), "Use a new host binding file; never overwrite raw metadata")
    require(args.output.resolve() not in (args.binary.resolve(), args.native_metadata.resolve()), "Output aliases raw evidence")
    native = read(args.native_metadata)
    require(native["gpuExecuted"] and native["stage"] == "tessellation-evaluation", "Native TES metadata missing")
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT / "build-vt-release/bin") + ";" + environment["PATH"]
    result = subprocess.run([str(executable), "--hash", str(args.binary.resolve())],
        env=environment, capture_output=True, text=True, check=True)
    require(result.stdout.strip() == native["nativePayloadBlake3"], "Native BLAKE3 payload binding failed")
    header, records = decode_tes(args.binary)
    require(int(header[0, 0]) == native["records"], "Native record count mismatch")
    for row, name in ((1, "frameEpochWords"), (2, "worldPoseWords"), (3, "drawWords")):
        require(list(map(int, header[row])) == native[name], "Native header mismatch: "+name)
    require(native["completedEpoch"] >= native["submissionEpoch"], "Native fence completion missing")
    output = dict(native)
    output["captureSha256"] = digest(args.binary)
    output["rawNativeMetadata"] = {"path": str(args.native_metadata.resolve()), "sha256": digest(args.native_metadata)}
    output["hostBindingScope"] = "Hash verified from immutable completed native raw files; no invented capture/epoch or modification of raw inputs"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(json.dumps({"status": "native-tes-files-bound", "records": len(records),
                      "output": str(args.output.resolve())}))


if __name__ == "__main__":
    main()
