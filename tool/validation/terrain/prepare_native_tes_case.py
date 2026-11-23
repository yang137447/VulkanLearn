"""Bind a first actual junction patch to native raw TES capture. CPU only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from rvt_visual_oracle import digest, read

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, default=ROOT.parent/"p5s/C010")
    parser.add_argument("--manifest", type=Path, default=ROOT.parent/"p5r/C010/VT/00af1195dc7dfbb0/manifest.json")
    parser.add_argument("--shader", type=Path, default=ROOT/"artifacts/terrain-rvt-production/bounded-visual-tes-prepare-r2/actual-production-tes-instrumented.tese")
    parser.add_argument("--hash-tool-build", type=Path, default=ROOT/"artifacts/terrain-rvt-production/native-tes-capture-private-r2/build.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    project, manifest, shader = (path.resolve(strict=True) for path in (args.project,args.manifest,args.shader))
    tool = read(args.hash_tool_build)
    executable = Path(tool["executable"])
    assert digest(executable) == tool["executableSha256"]
    env = os.environ.copy()
    env["PATH"] = str(ROOT/"build-vt-release/bin")+";"+env["PATH"]
    def blake(path):
        return subprocess.run([str(executable),"--hash",str(path)],env=env,capture_output=True,text=True,check=True).stdout.strip()
    config = read(project/"config/config.json")
    resources = (project/config["resourcePath"]).resolve()
    scene_path = resources/config["initScene"]
    scene = read(scene_path)
    placement = next(item for item in scene["objects"] if item.get("name")=="Landscape_00")
    terrain_path = resources/placement["terrainPath"]
    terrain = read(terrain_path)
    assert terrain["gridResolution"] == 512 and terrain["chunkQuads"] == 128
    camera = read(ROOT/"artifacts/terrain-rvt-production/p5/tour-final-runtime-evidence-matrix-cpu-r2/capture-and-oracle-checklist.json")
    pose = next(item for item in camera["poses"] if item["name"]=="junction_0_0")
    publication = read(manifest)
    surface_index = next(index for index,s in enumerate(publication["production"]["surfaces"]) if s["surfaceId"]=="terrain|Landscape_00")
    inputs = {str(path):digest(path) for path in (scene_path,terrain_path,manifest,
        project/"config/config.json",project/"config/renderGraphConfig.json")}
    actual_file_hashes = {name:blake(Path(name)) for name in inputs}
    for obj in publication["production"]["objects"]:
        path = manifest.parent/obj["path"]
        assert path.stat().st_size == obj["bytes"]
        inputs[str(path)] = digest(path)
    import hashlib
    closure_hash=hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()
    case = {"kind":"native-terrain-test-capture-v1","cameraPosition":pose["position"],
        "cameraLookAt":pose["look_at"],"surfaceId":"terrain|Landscape_00","surfaceIndex":surface_index,
        "lodGroupId":"terrain|Landscape_00|3|3","firstPrimitive":16383,"primitiveCount":1,"capacity":512,
        "patchSelectionEvidence":"Actual native LOD0 BuildChunks order row-major 128x128 quads in chunk3,3; final patch is at the maxU/maxV junction. Runtime adapter must still verify real group/LOD/topology/bindings.",
        "resourceRoot":str(resources),"publicationManifest":str(manifest),
        "publicationSha256":digest(manifest),"publicationBlake3":blake(manifest),
        "instrumentedTesSource":str(shader),"instrumentedTesSha256":digest(shader),
        "instrumentedTesBlake3":blake(shader),"inputClosureSha256":closure_hash,
        "output":str(output/"raw"),"sourceInputs":inputs,"actualFileBlake3":actual_file_hashes,
        "scope":"One actual normal LOD0 patch; fine-base-residency must come from native frame. Empty/overflow/nonzero-fine or missing active radius is not a pass."}
    case_path=output/"junction-one-patch.json"
    case_path.write_text(json.dumps(case,indent=2),encoding="utf-8")
    argv=["{coherentR10NativeTerrainGame.exe}","--initial-scene",config["initScene"],
        "--worker-thread-count","2","--frame-limit","30","--present-mode","immediate","--no-dev-ui",
        "--validation","--exit-after-frames","180","--console-command-delay","1",
        "--console-command","camera pose "+" ".join(format(v,".12g") for v in pose["position"]+pose["look_at"])]
    (output/"launch.json").write_text(json.dumps({"gpuExecuted":False,"cwd":str(project),"argv":argv,
        "environment":{"VULKANLEARN_TERRAIN_CAPTURE_CASE":str(case_path)},
        "requirement":"Link thin nativeTerrainGame.cpp plus adapter against ordinary full R10 core built with R3 hook ABI and Dirac frame bridge; no R9 overlay.",
        "oracleCommands":[
            [sys.executable,"-B",str(ROOT/"tool/validation/terrain/seal_native_tes_readback.py"),
             "--binary","{raw.bin}","--native-metadata","{raw.native.json}","--native-build",str(args.hash_tool_build),
             "--output","{new.bound.json}"],
            [sys.executable,"-B",str(ROOT/"tool/validation/terrain/rvt_visual_oracle.py"),"--output","{tes-report.json}",
             "tes","--capture","{raw.bin}","--metadata","{new.bound.json}","--manifest",str(manifest)]]
        },indent=2),encoding="utf-8")
    print(json.dumps({"case":str(case_path),"launch":str(output/"launch.json"),"gpuExecuted":False}))


if __name__=="__main__":
    main()
