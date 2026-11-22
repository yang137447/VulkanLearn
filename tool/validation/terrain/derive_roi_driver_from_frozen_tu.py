"""Derive an ROI-request driver from the frozen preprocessed numeric TU.

The p3 numeric objects (`originalBaker.obj`, the nine dependency objects and
`runtime-support.a`) were compiled from a header snapshot in which
`terrainCompositedBakeGpu.h`, `rendererBackendVulkan.h` and `commonFunction.h`
differ from the current worktree. Compiling a fresh driver TU against today's
headers therefore produces an ABI-incompatible binary (0xC0000005 in the first
Vulkan call). This script instead edits the frozen preprocessed translation unit
recorded in build.json (`compiledInputSha256` verified), changing only:

  * the four numeric page-request rows, and
  * the page-count assertion (16 surfaces x 4 requests = 64 pages).

Everything else is byte-identical, so the produced pages come from exactly the
same nested classes, inline functions and flags as the 192 verified pages.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_lit_evidence as T

OLD_REQUESTS = """    for (uint32_t pageY : {0u, 31u, 63u})
        for (uint32_t pageX : {0u, 31u, 63u})
            requests.push_back({8192, 8192, 0, pageX, pageY, false, 1});
    requests.push_back({8192, 8192, 3, 3, 3, false, 4});
    requests.push_back({8192, 8192, 6, 0, 0, false, 4});
    requests.push_back({8192, 8192, 13, 0, 0, false, 4});"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--build", type=Path,
                        default=T.ROOT / "artifacts/terrain-rvt-production/p3-real-numeric-c008/build.json")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    build = T.read(options.build)
    closure = build["compilationClosures"]["tool/validation/terrain/composited_real_numeric_check.cpp"]
    source = Path(closure["compiledInput"])
    payload = source.read_bytes()
    if hashlib.sha256(payload).hexdigest() != closure["compiledInputSha256"]:
        raise SystemExit("Frozen preprocessed TU changed; refusing to derive a driver")
    text = payload.decode("utf-8", errors="strict")
    newline = "\r\n" if "\r\n" in text else "\n"
    old_requests = OLD_REQUESTS.replace("\n", newline)
    requests = T.read(options.requests)["requests"]
    if len(requests) > 64:
        raise SystemExit("Too many ROI requests for a bounded single run")
    new_rows = newline.join(
        "    requests.push_back({8192, 8192, %d, %d, %d, false, 4});" %
        (row["mip"], row["x"], row["y"]) for row in requests)
    if text.count(old_requests) != 1:
        raise SystemExit("Frozen request block not found exactly once")
    expected = 16 * len(requests)
    old_assertion = 'Check(sequence == 192, "Real numeric missing surface/page coverage");'
    new_assertion = ('Check(sequence == %d, "ROI numeric missing surface/page coverage");' % expected)
    if text.count(old_assertion) != 1:
        raise SystemExit("Frozen page-count assertion not found exactly once")
    edited = text.replace(old_requests, new_rows).replace(old_assertion, new_assertion)
    replaced_scope = 'report["scope"] = "real published Full16; 12 distinct pages per surface; full three-channel bytes";'
    if edited.count(replaced_scope) != 1:
        raise SystemExit("Frozen scope string not found exactly once")
    edited = edited.replace(
        replaced_scope,
        'report["scope"] = "real published Full16; %d ROI-requested pages per surface; frozen evaluator closure";'
        % len(requests))
    output = Path(options.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(edited.encode("utf-8"))
    record = {
        "frozenSource": str(source.resolve()),
        "frozenSourceSha256": closure["compiledInputSha256"],
        "derivedSource": str(output.resolve()),
        "derivedSourceSha256": hashlib.sha256(edited.encode()).hexdigest(),
        "bytesBefore": len(payload), "bytesAfter": len(edited.encode()),
        "edits": ["replace four numeric request rows with %d ROI rows" % len(requests),
                  "page-count assertion 192 -> %d" % expected,
                  "scope string"],
        "requests": requests,
        "expectedPages": expected,
        "frozenFlags": closure["flags"],
        "frozenObjectSha256": closure["objectSha256"],
        "abiNote": ("compiled from the frozen preprocessed TU so the header snapshot matches the "
                    "p3 objects; a fresh TU against today's terrainCompositedBakeGpu.h / "
                    "rendererBackendVulkan.h / commonFunction.h crashes in Vulkan init"),
    }
    T.write(output.with_suffix(output.suffix + ".json"), record)
    print(json.dumps({"derived": str(output.resolve()), "expectedPages": expected}))


if __name__ == "__main__":
    main()
