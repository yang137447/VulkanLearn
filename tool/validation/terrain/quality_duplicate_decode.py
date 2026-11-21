"""Decode bounded duplicate witnesses without changing the quality acceptance rule."""

import argparse
import json
import struct
from pathlib import Path

BASE_BYTES = 1606912
CAPACITY = 16384
DUPLICATES = 256
STRIDE = 48
BYTES = BASE_BYTES + DUPLICATES * STRIDE


def decode(data, metadata):
    if len(data) != BYTES:
        raise ValueError("Expected exact TVQ1 v2 diagnostic payload")
    header = struct.unpack_from("<64I", data)
    if header[:3] != (0x31515654, 2, CAPACITY):
        raise ValueError("Unknown diagnostic ABI")
    if metadata["completedEpoch"] < metadata["submissionEpoch"]:
        raise ValueError("Capture has not completed")
    for offset, field in ((8, "frame"), (10, "poseId"), (12, "worldGeneration"),
                          (14, "submissionEpoch")):
        if header[offset] | header[offset + 1] << 32 != metadata[field]:
            raise ValueError("Capture metadata identity mismatch: " + field)
    attempted, overflow = header[23:25]
    if attempted != header[4] or overflow != max(0, attempted - DUPLICATES):
        raise ValueError("Diagnostic claim accounting mismatch")
    names = metadata["surfaceIds"]

    def payload(words):
        floats = struct.unpack("<6f", struct.pack("<6I", *words[:6]))
        surface = words[6] & 65535
        return {"words": list(words), "uv": list(floats[:2]),
                "majorUv": list(floats[2:4]), "targetMip": floats[4],
                "bias": floats[5], "surfaceIndex": surface,
                "surfaceId": names[surface] if surface < len(names) else None,
                "tapCount": words[6] >> 16}

    witnesses = []
    observed = {}
    for index in range(min(attempted, DUPLICATES)):
        record = struct.unpack_from("<12I", data, BASE_BYTES + index * STRIDE)
        pixel, prior = record[:2]
        if pixel >= CAPACITY or prior < 1:
            raise ValueError("Invalid duplicate pixel/claim ordinal")
        first = struct.unpack_from("<8I", data, 256 + pixel * 32)
        if first[7] <= prior:
            raise ValueError("Incomplete claim accounting")
        observed.setdefault(pixel, set())
        if prior in observed[pixel]:
            raise ValueError("Duplicate diagnostic ordinal")
        observed[pixel].add(prior)
        witnesses.append({
            "pixel": [pixel % 128, pixel // 128], "claims": first[7],
            "claimOrdinal": prior + 1, "first": payload(first[:7]),
            "additional": payload(record[2:9]),
            "samplingPayloadBitIdentical": first[:7] == record[2:9],
            "additionalPrimitiveId": record[9] if record[9] != 0xffffffff else None,
            "firstPrimitiveId": None, "instanceIdentity": None,
            "additionalDepthBits": record[10], "finalDepthBits": record[11],
            "depthEqualsFinal": record[10] == record[11],
            "depthWinnerIdentity": None})
    duplicates = {}
    for pixel in range(CAPACITY):
        claims = struct.unpack_from("<I", data, 256 + pixel * 32 + 28)[0]
        if claims > 1:
            duplicates[pixel] = claims - 1
    if sum(duplicates.values()) != attempted:
        raise ValueError("Sample claims differ from duplicate counter")
    complete = not overflow and all(
        observed.get(pixel, set()) == set(range(1, extra + 1))
        for pixel, extra in duplicates.items())
    return {"binaryAbi": "TVQ1-final-results-v2", "frame": metadata["frame"],
            "captureSequence": header[19] | header[20] << 32,
            "visibleSamples": header[3], "resolvedSamples": header[6],
            "duplicateClaims": attempted, "overflow": overflow,
            "witnessesComplete": complete,
            "allSamplingPayloadsBitIdentical": complete and all(
                item["samplingPayloadBitIdentical"] for item in witnesses),
            "winnerProven": False, "deduplicationAuthorized": False,
            "witnesses": witnesses}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    metadata = options.metadata or options.binary.with_suffix(".json")
    report = decode(options.binary.read_bytes(), json.loads(metadata.read_text("utf-8")))
    encoded = json.dumps(report, indent=2, allow_nan=False)
    if options.output:
        options.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)


if __name__ == "__main__":
    main()
