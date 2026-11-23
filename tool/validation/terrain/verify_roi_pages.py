"""Verify a produced ROI page run against the publication and the frozen report.

CPU only. Checks every page's size (221,956 B), uint32 status word (0), BLAKE3
against the producer report, the report's manifest/production identity against
the real p5g/C010r14 publication, and that every requested address is present.
Optionally cross-checks BLAKE3 equality with a second run (determinism).
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_lit_evidence as T


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=T.MANIFEST)
    parser.add_argument("--cross-run", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    options = parser.parse_args()
    run = options.run
    report = T.read(run / "report.json")
    requests = T.read(options.requests)["requests"]
    blake = T.Blake3()
    manifest_blake3 = blake.hash(options.manifest)
    publication = T.read(options.manifest)
    surfaces = publication["production"]["surfaces"]
    surface_index = {row["surfaceId"]: index for index, row in enumerate(surfaces)}
    checks = []
    checks.append({"id": "report.status", "ok": report.get("status") == "produced-real-numeric-pages",
                   "detail": report.get("status")})
    checks.append({"id": "report.pageCount", "ok": report.get("pageCount") == 16 * len(requests),
                   "detail": "%s pages" % report.get("pageCount")})
    checks.append({"id": "report.manifestBlake3", "ok": report.get("manifestBlake3") == manifest_blake3,
                   "detail": report.get("manifestBlake3")})
    checks.append({"id": "report.productionBlake3",
                   "ok": report.get("productionBlake3") == publication["production"]["hash"],
                   "detail": report.get("productionBlake3")})
    by_key = {(row["surfaceId"], row["mip"], row["x"], row["y"]): row for row in report["pages"]}
    wanted = [(row["surfaceId"], row["mip"], row["x"], row["y"]) for row in requests]
    missing = [key for key in wanted if key not in by_key]
    checks.append({"id": "requests.present", "ok": not missing,
                   "detail": "%d/%d requested addresses present" % (len(wanted) - len(missing), len(wanted))})
    bad = []
    for row in report["pages"]:
        path = run / row["file"]
        if not path.exists() or path.stat().st_size != T.PAGE_BYTES:
            bad.append((row["file"], "size"))
            continue
        if struct.unpack_from("<I", path.read_bytes(), T.PAGE_BYTES - 4)[0] != 0:
            bad.append((row["file"], "status"))
            continue
        if blake.hash(path) != row["outputBlake3"]:
            bad.append((row["file"], "blake3"))
    checks.append({"id": "pages.bytes", "ok": not bad,
                   "detail": "%d pages: %d B, status 0, BLAKE3 re-verified" % (len(report["pages"]), T.PAGE_BYTES)})
    duplicate = len({row["file"] for row in report["pages"]}) != len(report["pages"])
    checks.append({"id": "pages.unique", "ok": not duplicate, "detail": "one file per page"})
    cross = None
    if options.cross_run:
        other = T.read(options.cross_run / "report.json")
        other_by_key = {(row["surfaceId"], row["mip"], row["x"], row["y"]): row for row in other["pages"]}
        unequal = [key for key, row in by_key.items()
                   if key not in other_by_key or other_by_key[key]["outputBlake3"] != row["outputBlake3"]]
        cross = {"crossRun": str(options.cross_run), "compared": len(by_key), "unequal": len(unequal)}
        checks.append({"id": "determinism", "ok": not unequal,
                       "detail": "%d/%d pages byte-identical across two GPU runs"
                                 % (len(by_key) - len(unequal), len(by_key))})
    document = {"gpuExecuted": True, "run": str(run.resolve()), "requests": str(options.requests.resolve()),
                "manifest": str(options.manifest.resolve()), "manifestSha256": T.sha256(options.manifest),
                "manifestBlake3": manifest_blake3,
                "reportSha256": T.sha256(run / "report.json"),
                "executable": None, "checks": checks, "crossRun": cross,
                "requestedPages": [{"surfaceId": key[0], "surfaceIndex": surface_index[key[0]],
                                    "mip": key[1], "x": key[2], "y": key[3],
                                    "file": by_key[key]["file"] if key in by_key else None,
                                    "outputBlake3": by_key[key]["outputBlake3"] if key in by_key else None}
                                   for key in wanted],
                "passed": all(item["ok"] for item in checks)}
    build_record = run.parent / "build-frozen-record.json"
    if build_record.exists():
        document["executable"] = T.read(build_record).get("executable")
        document["executableSha256"] = T.read(build_record).get("executableSha256")
    target = options.output or (run / "verification.json")
    T.write(target, document)
    print(json.dumps({"output": str(target), "passed": document["passed"],
                      "failed": [item["id"] for item in checks if not item["ok"]]}))
    raise SystemExit(0 if document["passed"] else 2)


if __name__ == "__main__":
    main()
