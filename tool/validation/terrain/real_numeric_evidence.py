import hashlib
import json
from pathlib import Path
import struct
import subprocess
import re


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(manifest):
    manifest = Path(manifest).resolve()
    document = json.loads(manifest.read_text(encoding="utf-8"))
    surfaces = document["production"]["surfaces"]
    if len(surfaces) != 16 or len({surface["surfaceId"] for surface in surfaces}) != 16:
        raise RuntimeError("Real numeric requires exactly 16 unique published surfaces")
    modes = {surface["weightSource"] for surface in surfaces}
    if len(modes) != 1 or next(iter(modes)) not in (0, 1):
        raise RuntimeError("Publication must contain one real weight mode")
    mode = next(iter(modes))
    layouts = {surface["surfaceId"]: surface for surface in document["surfaces"]}
    for surface in surfaces:
        layout = layouts[surface["surfaceId"]]
        if layout["width"] != 8192 or layout["height"] != 8192 or layout["addressMode"] != "clamp" or \
                surface["parameters"]["heightBlendStrength"] != 3 or \
                surface["parameters"]["frozenHeightBlendFade"] != 1:
            raise RuntimeError("Real numeric input layout or height-blend differs")
        if mode == 1:
            binding = surface["inputs"]
            local_id = binding[3] if isinstance(binding, list) else binding["localLayerWeights"]
            objects = {item["objectId"]: item for item in document["production"]["objects"]}
            if not local_id or objects[local_id]["image"]["format"] != "r32-float":
                raise RuntimeError("Local mode requires actual published R32 weight objects")
    files = {str(manifest): digest(manifest)}
    for item in document["production"]["objects"]:
        path = manifest.parent / item["path"]
        if path.stat().st_size != item["bytes"]:
            raise RuntimeError("Published raw object byte size changed: " + str(path))
        files[str(path)] = digest(path)
    return {
        "manifest": str(manifest), "weightSource": mode,
        "surfaceIds": sorted(layouts), "objects": len(document["production"]["objects"]),
        "filesSha256": files,
    }


def publication_shaders(manifest):
    manifest = Path(manifest)
    document = json.loads(manifest.read_text(encoding="utf-8"))
    generator = document["production"]["generator"]
    artifacts = {}
    for item in document["production"]["objects"]:
        if item["objectId"] not in generator["compiledArtifacts"]:
            continue
        path = manifest.parent / item["path"]
        assembly = subprocess.run(["spirv-dis", "--no-header", str(path)], check=True,
                                  capture_output=True, text=True, errors="replace").stdout
        match = re.search(r"OpExecutionMode\s+\S+\s+LocalSize\s+(\d+)\s+(\d+)\s+(\d+)", assembly)
        if not match:
            raise RuntimeError("Published compute artifact has no supported workgroup layout")
        size = tuple(map(int, match.groups()))
        name = "terrainCompositedBakeWorld" if size == (64, 1, 1) else \
            "terrainCompositedBakeWorldPadding" if size == (8, 8, 1) else None
        if name is None:
            raise RuntimeError("Unexpected published production compute layout")
        role = "debug" if "OpString" in assembly else "spv"
        key = name + "_comp." + role
        if key in artifacts:
            raise RuntimeError("Ambiguous published production artifact role")
        artifacts[key] = {"objectId": item["objectId"], "path": str(path), "sha256": digest(path)}
    if len(artifacts) != 4:
        raise RuntimeError("Published real numeric requires exactly World+Padding runtime/debug outputs")
    return artifacts


def expected_keys(source):
    pages = [(0, page_x, page_y) for page_y in (0, 31, 63) for page_x in (0, 31, 63)]
    pages += [(3, 3, 3), (6, 0, 0), (13, 0, 0)]
    return {(surface, mip, page_x, page_y)
            for surface in source["surfaceIds"] for mip, page_x, page_y in pages}


def read_role(directory, source):
    report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
    logs = "\n".join((directory / name).read_text(encoding="utf-8")
                     for name in ("stdout.log", "stderr.log"))
    if report.get("error") or report.get("status") != "produced-real-numeric-pages" or \
            "Validation Error:" in logs or "VUID-" in logs:
        raise RuntimeError("Invalid producer/reference: " + str(directory))
    rows = report.get("pages", [])
    keys = [(row["surfaceId"], row["mip"], row["x"], row["y"]) for row in rows]
    if len(rows) != 192 or len(set(keys)) != 192 or set(keys) != expected_keys(source):
        raise RuntimeError("Producer/reference omitted or duplicated real surface/page coverage")
    results = {}
    for row, key in zip(rows, keys):
        if row["status"] != 0 or row["weightSource"] != source["weightSource"] or \
                row["heightBlend"] != 3 or row["areaSamples"] != (1 if row["mip"] == 0 else 4):
            raise RuntimeError("Producer/reference status or actual parameters differ")
        data = (directory / row["file"]).read_bytes()
        if len(data) != 221956 or struct.unpack_from("<I", data, 221952)[0] != 0:
            raise RuntimeError("Incomplete real output or nonzero stored GPU status")
        results[key] = (row, data)
    return report, results


def compare_roles(directory, source):
    original_report, original = read_role(directory / "original", source)
    candidate_report, candidate = read_role(directory / "candidate", source)
    if original_report["manifestBlake3"] != candidate_report["manifestBlake3"] or \
            original_report["productionBlake3"] != candidate_report["productionBlake3"]:
        raise RuntimeError("Reference and candidate did not restore identical actual closure")
    comparisons = []
    for key in sorted(expected_keys(source)):
        reference = original[key][1]
        actual = candidate[key][1]
        maximum = [0, 0, 0]
        above = [0, 0, 0]
        examples = []
        for offset in range(221952):
            difference = abs(actual[offset] - reference[offset])
            channel = offset // 73984
            maximum[channel] = max(maximum[channel], difference)
            if difference > 1:
                above[channel] += 1
                if len(examples) < 12:
                    texel = offset % 73984 // 4
                    examples.append({"stored": [texel % 136, texel // 136],
                                     "channel": channel, "component": offset % 4,
                                     "candidate": actual[offset], "originalGpu": reference[offset]})
        comparisons.append({
            "surfaceId": key[0], "mip": key[1], "x": key[2], "y": key[3],
            "bytesCompared": 221952, "maximumChannelError": maximum,
            "channelBytesAboveOne": above, "examples": examples,
            "candidateOutputSha256": hashlib.sha256(actual).hexdigest(),
            "originalOutputSha256": hashlib.sha256(reference).hexdigest(),
        })
    return {
        "referenceValid": True, "pagesCompared": len(comparisons),
        "totalChannelBytesCompared": len(comparisons) * 221952,
        "maximumChannelError": [max(row["maximumChannelError"][channel] for row in comparisons)
                                for channel in range(3)],
        "bytesAboveOne": sum(sum(row["channelBytesAboveOne"]) for row in comparisons),
        "comparisons": comparisons,
    }
