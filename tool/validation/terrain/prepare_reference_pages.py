"""Bind real original-evaluator page bytes to the observed ROI slot footprint.

CPU only. The only page producer that exists today is
`p3-real-numeric-c008/.../originalRealNumeric.exe`, which baked 12 fixed
(surface, mip, x, y) requests per surface against the real C008 publication.
This script resolves every observed footprint slot against that set, verifies
size/status/BLAKE3 and the publication-input equivalence with p5g/C010r14, and
copies the bytes unchanged. It never fabricates a page and never substitutes a
coarser or finer page for a missing one.
"""

import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_lit_evidence as T


def resolve_page_source_manifest(page_source):
    launch = T.read(Path(page_source) / "launch.json")
    command = launch["command"]
    manifest = Path(command[1]).resolve()
    if not manifest.exists():
        raise ValueError("Original numeric page source manifest missing: " + str(manifest))
    return manifest


def prepare(footprint, page_source, destination_pages, manifest, blake, report_path=None,
            extra_reports=None):
    """Copy every resolvable footprint slot; return entries plus missing slots.

    `extra_reports` lets the ROI page driver (`roiRealNumeric.exe`) contribute
    additional real pages beside the fixed 192-page numeric run. Every extra
    report is verified the same way (size, status word, BLAKE3, publication
    identity); no page is ever synthesised.
    """
    page_source = Path(page_source)
    destination_pages = Path(destination_pages)
    report = T.read(report_path or (page_source / "report.json"))
    pages = {}
    for entry in report["pages"]:
        pages[(entry["surfaceId"], entry["mip"], entry["x"], entry["y"])] = (page_source, entry)
    for extra in extra_reports or []:
        extra = Path(extra)
        extra_report = T.read(extra)
        if extra_report.get("gpuExecuted") is not True:
            raise ValueError("Extra page report is not a GPU-executed producer run: " + str(extra))
        if extra_report.get("manifestBlake3") != blake.hash(manifest):
            raise ValueError("Extra page report was produced from a different publication: " + str(extra))
        for entry in extra_report["pages"]:
            key = (entry["surfaceId"], entry["mip"], entry["x"], entry["y"])
            if key in pages:
                continue
            pages[key] = (extra.parent, entry)
    source_manifest = resolve_page_source_manifest(page_source)
    equivalence = T.publication_equivalence(source_manifest, manifest)
    if report.get("gpuExecuted") is not True or report.get("pageCount") != 192:
        raise ValueError("Original page report is not the completed 192-page numeric run")
    destination_pages.mkdir(parents=True, exist_ok=True)
    entries, missing = [], []
    for slot in sorted(footprint):
        identity = footprint[slot]
        key = (identity["surfaceId"], identity["mip"], identity["x"], identity["y"])
        resolved = pages.get(key)
        if resolved is None:
            missing.append({"actualPhysicalSlot": int(slot), "surfaceId": key[0],
                            "mip": int(key[1]), "x": int(key[2]), "y": int(key[3]),
                            "reason": "no real original-evaluator page was ever baked for this address"})
            continue
        origin, entry = resolved
        source = Path(origin) / entry["file"]
        if source.stat().st_size != T.PAGE_BYTES:
            raise ValueError("Original page size differs: " + str(source))
        payload = source.read_bytes()
        status = struct.unpack_from("<I", payload, T.PAGE_BYTES - 4)[0]
        if status != 0:
            raise ValueError("Original page status != 0: " + str(source))
        blake3 = blake.hash(source)
        if blake3 != entry["outputBlake3"]:
            raise ValueError("Original page BLAKE3 differs from its producer report: " + str(source))
        target_name = "page-slot%03d.bin" % slot
        target = destination_pages / target_name
        shutil.copyfile(source, target)
        if blake.hash(target) != blake3 or T.sha256(target) != T.sha256(source):
            raise ValueError("Copied original page changed: " + str(target))
        entries.append({"actualPhysicalSlot": int(slot), "path": str(target.resolve()),
                        "blake3": blake3, "surfaceId": key[0], "mip": int(key[1]),
                        "x": int(key[2]), "y": int(key[3]),
                        "sourceFile": str(source.resolve()), "sourceSha256": T.sha256(source),
                        "bytes": T.PAGE_BYTES})
    return {"entries": entries, "missing": missing, "equivalence": equivalence,
            "sourceManifest": str(source_manifest), "sourceReport": str(Path(report_path or page_source / "report.json")),
            "referencePageSetSha256": T.page_set_sha256(entries) if entries else None}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--footprint", type=Path, required=True,
                        help="lit-case-plan.json produced by prepare_lit_case.py")
    parser.add_argument("--page-source", type=Path, default=T.PAGE_SOURCE)
    parser.add_argument("--manifest", type=Path, default=T.MANIFEST)
    parser.add_argument("--pages", type=Path, default=None)
    parser.add_argument("--extra-report", type=Path, action="append", default=None,
                        help="additional real producer report (e.g. page-driver/run-001/report.json)")
    parser.add_argument("--output", type=Path, default=None)
    options = parser.parse_args()
    plan = T.read(options.footprint)
    footprint = {int(slot): identity for slot, identity in plan["footprint"].items()}
    pages = options.pages or (Path(plan["caseDirectory"]) / "pages")
    blake = T.Blake3()
    result = prepare(footprint, options.page_source, pages, options.manifest, blake,
                     extra_reports=options.extra_report)
    result["footprintSource"] = str(options.footprint.resolve())
    output = options.output or (Path(plan["caseDirectory"]) / "reference-pages.json")
    T.write(output, result)
    print(json.dumps({"output": str(output), "pages": len(result["entries"]),
                      "missing": len(result["missing"]),
                      "referencePageSetSha256": result["referencePageSetSha256"]}))
    if result["missing"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
