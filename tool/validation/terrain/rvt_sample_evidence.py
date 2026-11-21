"""Recompute current quality from fence-completed GPU page/slot samples."""

import math

from rvt_delivery import require


def quality_counts(document, event, surfaces):
    require(document.get("protocol") == 1 and document.get("source") == "gpu-page-table-readback",
            "Quality requires actual GPU page table readback")
    require(document.get("captureScope") == "all-fragment-taps",
            "Center-only feedback is insufficient for full-fragment quality")
    require(document["completedEpoch"] >= document["submissionEpoch"] and
            all(document[field] == event[field] for field in ("frame", "worldGeneration", "poseId")),
            "GPU sample epoch/frame/pose does not match observation")
    samples = document["samples"]
    require(isinstance(samples, list) and bool(samples), "No visible GPU samples")
    require(document.get("samplingDomain") == "feedback-visible-terrain" and
            document.get("feedbackExtent") == [128, 128] and document.get("sampleCapacity") == 16384,
            "Quality domain must be the bounded World feedback grid, not every screen fragment")
    require(document.get("sampleCount") == document.get("terrainVisibleCount") == len(samples)
            and len(samples) <= 16384 and document.get("overflowCount") == 0 and
            document.get("droppedTerrainSamples") == 0, "Incomplete or overflowing feedback capture")
    pixels = [tuple(sample["feedbackPixel"]) for sample in samples]
    require(len(set(pixels)) == len(pixels) and all(
        len(pixel) == 2 and all(type(value) is int and 0 <= value < 128 for value in pixel)
        for pixel in pixels), "Feedback coordinates duplicated or outside capture domain")
    counts = {"visibleSamples": len(samples), "validAtTargetPlusBias": 0,
              "effectiveMipBias": 0, "unresolved": 0, "incorrectIdentity": 0,
              "rootOnlySamples": 0, "uninitializedDraws": document["uninitializedDraws"]}
    for sample in samples:
        surface = surfaces.get(sample["surfaceId"])
        require(surface is not None, "GPU sample surface absent from package")
        target, bias = sample["targetMip"], sample["effectiveMipBias"]
        require(type(target) in (int, float) and math.isfinite(target) and
                type(bias) is int and 0 <= bias <= 1, "Invalid target/effective mip")
        counts["effectiveMipBias"] = max(counts["effectiveMipBias"], bias)
        maximum = int(math.floor(math.log2(max(surface["width"], surface["height"]))))
        require(sample["maxMip"] == maximum and 0 <= target <= maximum,
                "GPU sample virtual mip layout differs from publication")
        require(sample["expectedContentIdentity"] == surface["contentHash"],
                "Expected sampled identity differs from installed publication")
        taps = sample.get("taps")
        require(isinstance(taps, list) and 1 <= len(taps) <= 8
                and sample.get("tapCount") == len(taps), "Missing/overflowing actual captured-sample taps")
        missing, incorrect, good, root_only = False, False, True, target < 12
        requested_low = min(maximum, int(math.floor(target)) + bias)
        for tap in taps:
            for label, requested in (("low", requested_low), ("high", min(maximum, requested_low + 1))):
                result = tap[label]
                require(result["requestedMip"] == requested, "Sample requested pair differs from footprint")
                traversal = result["traversal"]
                require(isinstance(traversal, list) and traversal, "Missing actual page table traversal")
                require([row["mip"] for row in traversal] == list(
                    range(requested, requested + len(traversal))) and traversal[-1]["mip"] <= maximum,
                    "Page table traversal skipped mip or exceeded layout")
                require(all(row["entry"] == 0 for row in traversal[:-1]),
                        "Traversal ignored an earlier valid page")
                last = traversal[-1]
                for row in traversal:
                    width, height = max(1, surface["width"] >> row["mip"]), max(1, surface["height"] >> row["mip"])
                    require(type(row["entry"]) is int and row["entry"] >= 0 and
                            0 <= row["x"] < math.ceil(width / 128) and
                            0 <= row["y"] < math.ceil(height / 128),
                            "Invalid actual page table address")
                if last["entry"] == 0:
                    require(last["mip"] == maximum, "Unresolved traversal stopped before root")
                    missing, good, root_only = True, False, False
                    continue
                require(result["entry"] == last["entry"] and result["resolvedMip"] == last["mip"]
                        and result["x"] == last["x"] and result["y"] == last["y"],
                        "Resolved page disagrees with raw GPU traversal")
                owner = result["slotOwner"]
                matches = owner is not None and owner["slot"] == last["entry"] - 1 and all(
                    owner[field] == expected for field, expected in (
                        ("surfaceId", sample["surfaceId"]), ("mip", last["mip"]), ("x", last["x"]),
                        ("y", last["y"]), ("contentIdentity", surface["contentHash"]),
                        ("revision", sample["expectedRevision"]), ("worldGeneration", event["worldGeneration"])))
                incorrect |= not matches
                good &= matches and last["mip"] <= min(maximum, requested - bias + 1)
                root_only &= last["mip"] == maximum
        counts["unresolved"] += int(missing)
        counts["incorrectIdentity"] += int(incorrect)
        counts["rootOnlySamples"] += int(root_only)
        counts["validAtTargetPlusBias"] += int(good and not missing and not incorrect)
    for field, value in counts.items():
        require(event[field] == value, f"Quality aggregate differs from raw GPU samples: {field}")
    return counts
