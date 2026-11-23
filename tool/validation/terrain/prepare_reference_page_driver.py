"""Emit the exact build/run plan that closes the lit-ROI reference-page gap.

The only real original-evaluator producer that exists
(`originalRealNumeric.exe`) hardcodes 12 page addresses per surface; the ROI at
`junction_0_0` additionally samples four mip1 corner pages that were never
baked. This script writes the request list those slots need and the exact
compile/link/run commands for `composited_real_numeric_roi_check.cpp`, reusing
the frozen objects, runtime support archive and link response that already
produced the 192 verified pages.

It does not compile anything: that is a GPU-stage action (and a build), both of
which are outside this CPU-only pass. Nothing here creates page bytes.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_lit_evidence as T

DRIVER_SOURCE = Path(__file__).resolve().parent / "composited_real_numeric_roi_check.cpp"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, required=True,
                        help="lit-case-plan.json produced by prepare_lit_case.py")
    parser.add_argument("--page-source", type=Path, default=T.PAGE_SOURCE)
    parser.add_argument("--output", type=Path, default=None)
    options = parser.parse_args()
    plan = T.read(options.plan)
    page_source = options.page_source.resolve()
    build_root = page_source.parents[3]
    build_path = build_root / "build.json"
    if not build_path.exists():
        build_path = page_source.parents[1] / "build.json"
    build = T.read(build_path)
    output = options.output or (Path(plan["caseDirectory"]).parent / "page-driver")
    output.mkdir(parents=True, exist_ok=True)

    missing = plan["missingOriginalPages"]
    requests = [{"surfaceId": row["surfaceId"], "mip": int(row["mip"]),
                 "x": int(row["x"]), "y": int(row["y"])} for row in missing]
    request_document = {"manifest": str(T.MANIFEST.resolve()), "weightSource": 0,
                        "motive": ("original-evaluator pages for the exact ROI-sampled slots that "
                                   "the fixed 192-page numeric run never baked"),
                        "requests": requests}
    requests_path = output / "requests.json"
    T.write(requests_path, request_document)

    closures = build["compilationClosures"]
    flags = closures["tool/validation/terrain/composited_real_numeric_check.cpp"]["flags"]
    objects = [Path(record["object"]) for name, record in closures.items()]
    driver_object = Path(closures["tool/validation/terrain/composited_real_numeric_check.cpp"]["object"])
    link_objects = [path for path in objects if path != driver_object]
    baker_object = build_root / "originalBaker.obj"
    support = build_root / "runtime-support.a"
    response = build_root / "libraries.rsp"
    compiler = shutil.which("g++")
    issues = []
    for label, path, expected in (
            ("runtime-support.a", support, build.get("runtimeSupportSha256")),
            ("originalBaker.obj", baker_object, build.get("originalObjectSha256"))):
        if not path.exists():
            issues.append(label + " missing: " + str(path))
        elif expected and T.sha256(path) != expected:
            issues.append(label + " hash changed: " + str(path))
    for path in link_objects:
        if not path.exists():
            issues.append("frozen object missing: " + str(path))
    if not response.exists():
        issues.append("link response missing: " + str(response))
    if compiler is None:
        issues.append("g++ not resolvable on PATH")

    executable = output / "roiRealNumeric.exe"
    compile_command = ([compiler or "g++"] + flags +
                       ["-c", str(DRIVER_SOURCE), "-o", str(output / "composited_real_numeric_roi_check.cpp.obj")])
    link_command = ([compiler or "g++"] + [str(path) for path in link_objects] +
                    [str(output / "composited_real_numeric_roi_check.cpp.obj"), str(baker_object),
                     "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "roiRealNumeric.link.map"),
                     "-o", str(executable), "@" + str(response)])
    run_directory = output / "run-001"
    run_command = ([str(executable), str(T.MANIFEST.resolve()), "0", str(requests_path),
                    str(run_directory)])
    surfaces = T.read(T.MANIFEST)["production"]["surfaces"]
    surface_index = {row["surfaceId"]: index for index, row in enumerate(surfaces)}
    expected = []
    for row in requests:
        name = ("page-surface%d-mip%d-%d-%d.bin" %
                (surface_index[row["surfaceId"]], row["mip"], row["x"], row["y"]))
        expected.append(str((run_directory / name).resolve()))
    expected.append(str((run_directory / "report.json").resolve()))
    document = {
        "gpuExecuted": False, "compiled": False,
        "scope": ("Request-list-driven rebuild of the independent original evaluator; the four "
                  "missing ROI slots are mip1 corner pages that no existing artefact contains"),
        "plan": str(Path(options.plan).resolve()),
        "requests": str(requests_path.resolve()), "requestCount": len(requests),
        "driverSource": str(DRIVER_SOURCE.resolve()),
        "driverSourceSha256": T.sha256(DRIVER_SOURCE),
        "compiler": compiler,
        "flags": flags,
        "frozenObjects": [str(path) for path in link_objects],
        "bakerObject": str(baker_object), "runtimeSupport": str(support),
        "linkResponse": str(response),
        "compileCommand": compile_command,
        "linkCommand": link_command,
        "runCommand": run_command,
        "expectedOutputs": expected,
        "afterRun": [
            "py -3 tool/validation/terrain/prepare_lit_case.py --pose junction_0_0 --name junction-0-0",
            "py -3 tool/validation/terrain/make_lit_masks.py --plan "
            "artifacts/terrain-rvt-production/final-delivery/lit/case-junction-0-0/lit-case-plan.json "
            "--name junction-0-0",
            "verify the new pages against this plan's manifest identity before binding them",
        ],
        "problems": issues,
        "status": "ready-to-build" if not issues else "blocked",
    }
    target = output / "build-plan.json"
    T.write(target, document)
    print(json.dumps({"buildPlan": str(target), "requests": len(requests),
                      "status": document["status"], "problems": issues}))
    if issues:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
