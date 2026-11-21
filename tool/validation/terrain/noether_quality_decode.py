"""Bounded TVQ1 decoder. Binary values are observations, never reconstructed walks."""

import argparse
import json
import math
import struct
from pathlib import Path

MAGIC = 0x31515654
VERSION = 1
CAPACITY = 16384
SAMPLE_WORD = 64
RESULT_WORD = 131136
OWNER_WORD = 393280
SURFACE_WORD = 401472
PAYLOAD_BYTES = 1606912
SAMPLE_STRIDE = 32
OWNER_STRIDE = SURFACE_STRIDE = 64


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def join(low, high):
    return low | high << 32


class Capture:
    def __init__(self, payload, metadata, verified_surfaces=None):
        version = struct.unpack_from("<I", payload, 4)[0] if len(payload) >= 8 else 0
        require(version in (1, 2) and len(payload) ==
                PAYLOAD_BYTES + (12288 if version == 2 else 0), "TVQ1 payload size")
        self.payload = memoryview(payload)
        self.metadata = metadata
        self.header = struct.unpack_from("<64I", self.payload)
        header = self.header
        require(header[:3] == (MAGIC, version, CAPACITY), "TVQ1 magic/version/capacity")
        require(1 <= header[18] <= 16, "TVQ1 surface count")
        require(join(header[14], header[15]) > 0, "TVQ1 missing submission epoch")
        require(header[16] > 0 and header[17] > 0, "TVQ1 render extent")
        self.frame = join(header[8], header[9])
        self.pose = join(header[10], header[11])
        self.generation = join(header[12], header[13])
        self.epoch = join(header[14], header[15])
        require(metadata.get("protocol") == 1 and
                metadata.get("source") == "gpu-page-table-readback", "TVQ1 readback source")
        require(metadata["completedEpoch"] >= self.epoch and
                metadata["submissionEpoch"] == self.epoch, "TVQ1 incomplete/mismatched epoch")
        require(all(metadata[name] == observed for name, observed in
                    (("frame", self.frame), ("poseId", self.pose),
                     ("worldGeneration", self.generation))), "TVQ1 draw identity mismatch")
        names = metadata["surfaceIds"]
        require(len(names) == header[18] and len(set(names)) == len(names),
                "TVQ1 surface ID dictionary")
        self.surfaces = []
        for index, name in enumerate(names):
            row = struct.unpack_from("<16I", self.payload, (SURFACE_WORD + index * 16) * 4)
            maximum, repeat = row[3] & 255, bool(row[3] & 256)
            require(row[0] == index and 1 <= row[1] <= 8192 and 1 <= row[2] <= 8192
                    and maximum == max(row[1], row[2]).bit_length() - 1
                    and row[3] & ~511 == 0 and join(row[4], row[5]) == self.generation,
                    "TVQ1 surface layout/generation")
            identity = bytes(self.payload[(SURFACE_WORD + index * 16 + 8) * 4:
                                          (SURFACE_WORD + index * 16 + 16) * 4]).hex()
            surface = {"surfaceId": name, "width": row[1], "height": row[2], "maxMip": maximum,
                       "repeat": repeat, "expectedRevision": join(row[6], row[7]),
                       "expectedContentIdentity": identity}
            if verified_surfaces is not None:
                verified = verified_surfaces[name]
                require(all(surface[field] == verified[expected] for field, expected in
                            (("width", "width"), ("height", "height"),
                             ("expectedContentIdentity", "contentHash"))),
                        "TVQ1 expected identity differs from verified publication/refresh")
            self.surfaces.append(surface)

    def owner(self, entry):
        if entry == 0:
            return None
        require(1 <= entry <= 512, "TVQ1 invalid page-table entry")
        offset = (OWNER_WORD + (entry - 1) * 16) * 4
        row = struct.unpack_from("<16I", self.payload, offset)
        require(row[0] in (0, 1), "TVQ1 invalid owner state")
        if row[0] == 0:
            return None
        names = self.metadata["surfaceIds"]
        return {"slot": entry - 1, "surfaceId": names[row[1]] if row[1] < len(names) else None,
                "mip": row[2], "x": row[3] & 65535, "y": row[3] >> 16,
                "worldGeneration": join(row[4], row[5]), "revision": join(row[6], row[7]),
                "contentIdentity": bytes(self.payload[offset + 32:offset + 64]).hex()}

    def samples(self):
        for pixel in range(CAPACITY):
            values = struct.unpack_from("<6f2I", self.payload, (SAMPLE_WORD + pixel * 8) * 4)
            claims = values[7]
            if not claims:
                continue
            require(claims == 1, "TVQ1 duplicate visible pixel")
            surface_index, taps = values[6] & 65535, values[6] >> 16
            require(surface_index < len(self.surfaces) and 1 <= taps <= 8,
                    "TVQ1 surface/tap count")
            require(all(math.isfinite(value) for value in values[:6]), "TVQ1 nonfinite footprint")
            surface = self.surfaces[surface_index]
            maximum = surface["maxMip"]
            target, bias = values[4:6]
            require(0 <= target <= maximum and bias in (0, 1), "TVQ1 target/bias")
            low = min(maximum, math.floor(target) + int(bias))
            result = {**surface, "uv": list(values[:2]), "majorUv": list(values[2:4]),
                      "feedbackPixel": [pixel % 128, pixel // 128], "targetMip": target,
                      "effectiveMipBias": int(bias), "tapCount": taps, "taps": []}
            for tap in range(taps):
                pair = {}
                for branch, label in enumerate(("low", "high")):
                    packed = struct.unpack_from("<I", self.payload,
                                                (RESULT_WORD + pixel * 16 + tap * 2 + branch) * 4)[0]
                    entry, mip = packed & 1023, packed >> 10 & 15
                    page_x, page_y = packed >> 14 & 63, packed >> 20 & 63
                    requested = min(maximum, low + branch)
                    require(packed & 1 << 26 and packed >> 28 == 0 and requested <= mip <= maximum,
                            "TVQ1 missing/invalid resolved result")
                    require(page_x < (max(1, surface["width"] >> mip) + 127) // 128
                            and page_y < (max(1, surface["height"] >> mip) + 127) // 128,
                            "TVQ1 invalid resolved page")
                    pair[label] = {"requestedMip": requested, "resolvedMip": mip,
                                   "x": page_x, "y": page_y, "entry": entry,
                                   "anchored": bool(packed & 1 << 27), "slotOwner": self.owner(entry)}
                result["taps"].append(pair)
            yield result

    def counts(self):
        header = self.header
        require(not any(header[index] for index in (4, 5, 21, 22)),
                "TVQ1 duplicate/invalid/dropped/overflow capture")
        counts = {"visibleSamples": 0, "validAtTargetPlusBias": 0, "effectiveMipBias": 0,
                  "unresolved": 0, "incorrectIdentity": 0, "rootOnlySamples": 0,
                  "uninitializedDraws": header[7]}
        for sample in self.samples():
            counts["visibleSamples"] += 1
            counts["effectiveMipBias"] = max(counts["effectiveMipBias"], sample["effectiveMipBias"])
            missing, incorrect, good, root = False, False, True, sample["targetMip"] < 12
            for tap in sample["taps"]:
                for result in tap.values():
                    owner = result["slotOwner"]
                    if not result["entry"] or owner is None:
                        missing, good, root = True, False, False
                        continue
                    matches = all(owner[field] == expected for field, expected in (
                        ("surfaceId", sample["surfaceId"]), ("mip", result["resolvedMip"]),
                        ("x", result["x"]), ("y", result["y"]),
                        ("contentIdentity", sample["expectedContentIdentity"]),
                        ("revision", sample["expectedRevision"]), ("worldGeneration", self.generation)))
                    incorrect |= not matches
                    good &= matches and result["resolvedMip"] <= min(
                        sample["maxMip"], result["requestedMip"] - sample["effectiveMipBias"] + 1)
                    root &= result["resolvedMip"] == sample["maxMip"]
            counts["unresolved"] += int(missing)
            counts["incorrectIdentity"] += int(incorrect)
            counts["rootOnlySamples"] += int(root)
            counts["validAtTargetPlusBias"] += int(good and not missing and not incorrect)
        require(counts["visibleSamples"] == header[3] == header[6] <= CAPACITY,
                "TVQ1 missing/dropped visible terrain")
        return counts

    def document_fields(self):
        header = self.header
        return {**self.metadata, "binaryAbi": f"TVQ1-final-results-v{header[1]}",
                "captureScope": "all-fragment-taps", "samplingDomain": "feedback-visible-terrain",
                "feedbackExtent": [128, 128], "renderExtent": list(header[16:18]),
                "sampleCapacity": CAPACITY, "sampleCount": header[6], "terrainVisibleCount": header[3],
                "overflowCount": header[22], "droppedTerrainSamples": header[21],
                "captureSequence": join(header[19], header[20]), "uninitializedDraws": header[7]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--expanded-output", type=Path)
    options = parser.parse_args()
    capture = Capture(options.binary.read_bytes(), json.loads(options.metadata.read_text("utf-8")))
    counts = capture.counts()
    if options.expanded_output:
        with options.expanded_output.open("w", encoding="utf-8") as stream:
            prefix = json.dumps(capture.document_fields(), ensure_ascii=True, separators=(",", ":"))
            stream.write(prefix[:-1] + ',"samples":[')
            for index, sample in enumerate(capture.samples()):
                stream.write(("," if index else "") + json.dumps(sample, separators=(",", ":")))
            stream.write("]}\n")
    print(json.dumps(counts, separators=(",", ":")))


if __name__ == "__main__":
    main()
