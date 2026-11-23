"""Decode Noether TerrainQualityAbi v1 directly without expanded sample JSON."""

import math
import struct

from rvt_delivery import require

MAGIC = 0x31515654
BYTES = 1606912
CAPACITY = 128 * 128
SAMPLE_OFFSET = 64 * 4
RESULT_OFFSET = SAMPLE_OFFSET + CAPACITY * 32
OWNER_OFFSET = RESULT_OFFSET + CAPACITY * 16 * 4
SURFACE_OFFSET = OWNER_OFFSET + 512 * 64


def join(low, high):
    return low | high << 32


def quality_binary_counts(data, event, surfaces):
    require(len(data) >= 256, "Unknown QualityGpu ABI header")
    words = struct.unpack_from("<64I", data)
    require(words[1] in (1, 2) and len(data) == BYTES + (12288 if words[1] == 2 else 0),
            "Unknown QualityGpu ABI byte count/version")
    require(words[0] == MAGIC and words[1] in (1, 2) and words[2] == CAPACITY,
            "Unknown QualityGpu magic/version/capacity")
    ids = event["surfaceIds"]
    require(isinstance(ids, list) and 0 < len(ids) <= 16 and len(ids) == len(set(ids))
            and set(ids) == set(surfaces) and words[18] == len(ids),
            "QualityGpu surface dictionary differs from installed publication")
    require(join(words[8], words[9]) == event["frame"]
            and str(join(words[10], words[11])) == str(event["poseId"])
            and join(words[12], words[13]) == event["worldGeneration"]
            and join(words[14], words[15]) == event["submissionEpoch"]
            and event["submissionEpoch"] > 0
            and event["completedEpoch"] >= event["submissionEpoch"],
            "QualityGpu frame/pose/draw epoch does not match completed capture")
    require(words[16] > 0 and words[17] > 0 and list(words[16:18]) == event["renderExtent"],
            "QualityGpu render extent differs")
    require(join(words[19], words[20]) == event["captureSequence"],
            "QualityGpu capture sequence differs")
    require(all(words[index] == 0 for index in (4, 5, 21, 22))
            and not any(words[23:]), "QualityGpu duplicate/invalid/dropped/overflow/reserved header")
    require(event.get("samplingDomain") == "feedback-visible-terrain"
            and event.get("feedbackExtent") == [128, 128]
            and event.get("captureScope") == "all-fragment-taps", "QualityGpu domain differs")
    descriptions = []
    for index, name in enumerate(ids):
        offset = SURFACE_OFFSET + index * 64
        surface = struct.unpack_from("<8I", data, offset)
        expected = surfaces[name]
        maximum = int(math.floor(math.log2(max(expected["width"], expected["height"]))))
        identity = data[offset + 32:offset + 64].hex()
        require(surface[0] == index and surface[1:3] == (expected["width"], expected["height"])
                and surface[3] & 255 == maximum and surface[3] & ~511 == 0 and maximum <= 13
                and join(surface[4], surface[5]) == event["worldGeneration"]
                and identity == expected["contentHash"], "QualityGpu expected surface identity differs")
        descriptions.append((surface, identity))
    counts = {"visibleSamples": 0, "validAtTargetPlusBias": 0, "effectiveMipBias": 0,
              "unresolved": 0, "incorrectIdentity": 0, "rootOnlySamples": 0,
              "uninitializedDraws": words[7]}
    for pixel in range(CAPACITY):
        sample = struct.unpack_from("<6f2I", data, SAMPLE_OFFSET + pixel * 32)
        if sample[7] == 0:
            continue
        counts["visibleSamples"] += 1
        index, taps = sample[6] & 65535, sample[6] >> 16
        target, bias_float = sample[4:6]
        require(sample[7] == 1 and index < len(ids) and 1 <= taps <= 8 and
                all(math.isfinite(value) for value in sample[:6]) and
                bias_float in (0, 1), "Malformed QualityGpu sample/tap claims")
        surface, identity = descriptions[index]
        maximum = surface[3] & 255
        require(0 <= target <= maximum, "QualityGpu target mip differs from layout")
        bias = int(bias_float)
        counts["effectiveMipBias"] = max(counts["effectiveMipBias"], bias)
        low = min(maximum, int(target) + bias)
        missing, wrong, good, root = False, False, True, target < 12
        for tap in range(taps):
            for branch in range(2):
                packed = struct.unpack_from("<I", data, RESULT_OFFSET + (pixel * 16 + tap * 2 + branch) * 4)[0]
                entry, mip = packed & 1023, packed >> 10 & 15
                page_x, page_y = packed >> 14 & 63, packed >> 20 & 63
                requested = min(maximum, low + branch)
                require(packed & (1 << 26) and packed >> 28 == 0 and requested <= mip <= maximum and
                        page_x < (max(1, surface[1] >> mip) + 127) // 128 and
                        page_y < (max(1, surface[2] >> mip) + 127) // 128,
                        "Malformed QualityGpu actual resolved address")
                if entry == 0 or entry > 512:
                    missing, good, root = True, False, False
                    continue
                owner_offset = OWNER_OFFSET + (entry - 1) * 64
                owner = struct.unpack_from("<8I", data, owner_offset)
                if owner[0] == 0:
                    missing, good, root = True, False, False
                    continue
                matches = owner[1] == index and owner[2] == mip and owner[3] == page_x | page_y << 16 and (
                    owner[4:8] == surface[4:8]) and data[owner_offset + 32:owner_offset + 64].hex() == identity
                wrong |= not matches
                good &= matches and mip <= min(maximum, requested - bias + 1)
                root &= mip == maximum
        counts["unresolved"] += int(missing)
        counts["incorrectIdentity"] += int(wrong)
        counts["rootOnlySamples"] += int(root)
        counts["validAtTargetPlusBias"] += int(good and not missing and not wrong)
    require(counts["visibleSamples"] > 0 and counts["visibleSamples"] == words[3] == words[6],
            "QualityGpu visible/resolved counters differ or no visible terrain")
    for field, value in counts.items():
        require(event[field] == value, f"QualityGpu count differs from independent raw decode: {field}")
    return counts


def decode_bound_capture(data, metadata, event, surfaces):
    require(metadata.get("protocol") == 1 and metadata.get("source") == "gpu-page-table-readback"
            and metadata.get("binaryAbi") in ("TVQ1-final-results-v1", "TVQ1-final-results-v2"),
            "QualityGpu requires actual WriteReadback metadata")
    require(len(data) >= 256, "Unknown QualityGpu ABI header")
    words = struct.unpack_from("<64I", data)
    require(words[1] in (1, 2) and len(data) == BYTES + (12288 if words[1] == 2 else 0)
            and metadata["binaryAbi"] == f"TVQ1-final-results-v{words[1]}",
            "QualityGpu ABI byte count/version differs")
    merged = {**metadata, **event}
    for field in ("frame", "poseId", "worldGeneration", "submissionEpoch", "completedEpoch"):
        require(field not in event or str(event[field]) == str(metadata[field]),
                f"Quality log/metadata differs: {field}")
    derived = {"renderExtent": list(words[16:18]),
               "captureSequence": join(words[19], words[20]),
               "samplingDomain": "feedback-visible-terrain", "feedbackExtent": [128, 128],
               "captureScope": "all-fragment-taps"}
    for field, value in derived.items():
        require(field not in merged or merged[field] == value, f"Quality log/header differs: {field}")
        merged[field] = value
    return quality_binary_counts(data, merged, surfaces)
