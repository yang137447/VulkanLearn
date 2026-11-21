"""Account for the actual Full16 package and deterministic GPU payload sizes."""

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-report", type=Path)
    parser.add_argument("--geometry-abi", type=Path)
    options = parser.parse_args()
    stage = options.stage.resolve()
    config = read(stage / "config/config.json")
    root = Path(config["resourcePath"])
    if not root.is_absolute():
        root = (stage / root).resolve()
    files, categories = [], defaultdict(int)
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        category = ("materialPayload" if path.suffix == ".vlcvt" else
                    "coverage" if ".coverage." in path.name else
                    "height" if ".height." in path.name else
                    "index" if path.name == "manifest.json" and "VT" in path.parts else
                    "dependency" if ".dependencies." in path.name else "authorAndDynamic")
        record = {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                  "mtimeNs": path.stat().st_mtime_ns, "category": category}
        if path.suffix != ".vlcvt":
            record["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        files.append(record)
        categories[category] += record["bytes"]
    surfaces = []
    for placement in read(root / config["initScene"])["objects"]:
        if placement["type"] != "terrain":
            continue
        terrain = read(root / placement["terrainPath"])
        grid = terrain["gridResolution"] + 1
        tiles = terrain["pageTilesPerSide"]
        surfaces.append({"terrain": placement["terrainPath"], "frameRgba32fBytes": grid * grid * 32,
                         "coverageBytes": tiles * tiles * 4 + 2 * (tiles * 128) ** 2 * 4,
                         "layerMappingBytes": 256 * 2 * 16, "gridResolution": grid - 1,
                         "package": terrain["compositedVT"]["packagePath"]})
    page_headers = sum(16 for item in files if item["category"] == "materialPayload")
    baseline = read(options.baseline_report) if options.baseline_report else None
    stats = baseline["sections"][0]["finalStats"] if baseline else None
    geometry = []
    if options.geometry_abi:
        abi = read(options.geometry_abi)
        for item in surfaces:
            terrain = read(root / item["terrain"])
            edge = terrain["chunkQuads"]
            chunks = (terrain["gridResolution"] // edge) ** 2
            for level in range(terrain["lodLevels"]):
                step = 2 ** level
                quads = edge // step
                base = quads if level == 0 else quads - 2
                triangles = 0 if level == 0 else 4 * quads * (step + 1)
                vertices = (quads + 1) ** 2 + (0 if level == 0 else 4 * edge)
                indices = base * base * 6 + triangles * 3
                patches = base * base * 4 + triangles * 4
                geometry.append({
                    "terrain": item["terrain"], "lod": level, "allResidentChunks": chunks,
                    "verticesPerChunk": vertices, "indicesPerChunk": indices,
                    "patchIndicesPerChunk": patches,
                    "payloadBytesAllChunks": chunks * (
                        vertices * abi["vertexStrideBytes"] +
                        (indices + patches) * abi["indexStrideBytes"])})
    result = {"kind": "rvt-p0-ledger", "scope": "Full16 independent fr closure; disk exact sizes, GPU payload arithmetic",
              "resourceRoot": str(root), "scene": config["initScene"], "disk": dict(categories),
              "diskTotalBytes": sum(categories.values()), "files": files, "surfaces": surfaces,
              "materialPayloadDataBytes": categories["materialPayload"] - page_headers,
              "materialPayloadHeaderBytes": page_headers,
              "frameTotalBytes": sum(item["frameRgba32fBytes"] for item in surfaces),
              "privateCoverageTotalBytes": sum(item["coverageBytes"] for item in surfaces),
              "sharedCoveragePayloadBytes": 32 * 32 * 4 + 2 * 4096 * 4096 * 4,
              "pageOutputWithStatusBytes": 136 * 136 * 12 + 4,
              "sourceMapTotalBytes": len(surfaces) * 256 * 2 * 16,
              "geometryAllResidentPayloadBytes": sum(item["payloadBytesAllChunks"] for item in geometry),
              "geometryBySurfaceLod": geometry,
              "geometryScope": "All-resident logical vertex + triangle index + tessellation patch index upper envelope from frozen builder and measured ABI; runtime streams a subset; not an observed simultaneous GPU allocation",
              "baselineServiceStats": stats,
              "baselineKnownImagePayloadBytes": (
                  int(stats["header"]["accountedGpuPayloadBytes"]) +
                  32 * 32 * 4 + 2 * 4096 * 4096 * 4 + len(surfaces) * 256 * 2 * 16
              ) if stats else None,
              "baselineAdditionalCategories": {
                  "compositedReadbackRingBytes": 128 * 128 * 4 * 4 * 2,
                  "feedbackRenderTargetsBytesPerSwapchainImage": 128 * 128 * 4 * 4,
                  "coverageUploadRingReservationBytes": (4096 + 8 * 128 * 128 * 4 * 2) * 2,
                  "geometryAndCpuSnapshots": "Accounted in process/WDDM peaks; separate all-resident CPU arithmetic supplied in geometry-ledger.json",
                  "uploadRings": "Per-surface stagingBytes observed in raw stats; CPU admission reserves 16 independent rings in V1",
                  "cpuQueuesAndIndex": "V1 manifest disk index measured; heap overhead/queues in whole-process peaks, not claimed exact per allocation",
                  "replacementOverlap": "Actual loadworld run includes old/new resources in PID memory peak; not inferred by doubling steady payload",
                  "driverAlignment": "WDDM samples include allocation/residency effects; no Vulkan allocator high-water claim"},
              "notes": ["Game V1 does not allocate production frame inputs; Full16 frame arithmetic is required V2 cost.",
                        "Disk bytes are not GPU allocation bytes; driver alignment is measured only through PID WDDM sampling.",
                        "Large immutable payload identity is checked by the V1 loader; ledger preserves path/size/mtime.",
                        "Existing V1 cache cap excludes source arrays and global coverage; use total ledger for V2 admission."]}
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("disk", "diskTotalBytes", "frameTotalBytes",
                                                  "privateCoverageTotalBytes", "sharedCoveragePayloadBytes")}, indent=2))


if __name__ == "__main__":
    main()
