"""Prepare private shader/cache test inputs; never launch Vulkan or edit a signed package."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "artifacts/terrain-rvt-production/geometry-seam-fix"
SOURCE = ROOT.parent / "p5g/tourr4"
STAGE = OUT / "stage"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {path.relative_to(root).as_posix(): digest(path) for path in root.rglob("*") if path.is_file()}


def main():
    baseline = inventory(SOURCE / "shader")
    if not STAGE.exists():
        shutil.copytree(SOURCE / "shader", STAGE / "shader")
        shutil.copytree(SOURCE / "config", STAGE / "config")
    added_sources = []
    for path in (ROOT / "shader/glsl").rglob("*"):
        destination = STAGE / path.relative_to(ROOT)
        if path.is_file() and not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            added_sources.append(path.relative_to(ROOT).as_posix())
    target = Path("shader/glsl/materialFunction/mf_terrainLayerBlend.glsl")
    old = (SOURCE / target).read_text(encoding="utf-8")
    new = (ROOT / target).read_text(encoding="utf-8")
    assert old.replace("const vec2 coverageUv = clamp(context.texCoord,", "const vec2 coverageUv = clamp(context.texCoord1,") == new
    shutil.copy2(ROOT / target, STAGE / target)
    (STAGE / "shader/glsl/__composed__").mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment["PATH"] = str(SOURCE) + ";" + environment["PATH"]
    commands = []

    def run(argv, label):
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=ROOT, env=environment, capture_output=True, text=True)
        (OUT / (label + ".log")).write_text(repr(argv) + "\n" + result.stdout + result.stderr, encoding="utf-8")
        commands.append({"argv": argv, "exitCode": result.returncode})
        if result.returncode:
            print(result.stdout + result.stderr)
        result.check_returncode()

    run([OUT / "compiler.exe", STAGE, SOURCE / "resources/VT/775359b06fde29e5/manifest.json",
         STAGE / target], "native-compiler")
    for model in ("SHADING_MODEL_DEFAULT_LIT", "SHADING_MODEL_UNLIT"):
        directory = STAGE / "link-inputs" / model
        directory.mkdir(exist_ok=True)
        for extension in ("vert", "frag"):
            text = (STAGE / "shader/glsl/pass" / ("terrainFeedback." + extension)).read_text(encoding="utf-8")
            text = text.replace("#version 450", "#version 450\n#extension GL_GOOGLE_include_directive : require\n#define MATERIAL_SHADING_MODEL " + model, 1)
            (directory / ("terrain." + extension)).write_text(text, encoding="utf-8")
    link_reports = []
    for directory in sorted((STAGE / "link-inputs").iterdir()):
        files = [directory / ("terrain." + extension) for extension in ("vert", "tesc", "tese", "frag")
                 if (directory / ("terrain." + extension)).is_file()]
        output = OUT / (directory.name + ".linked.spv")
        run(["glslangValidator", "-V", "--target-env", "vulkan1.4", "-l",
             "-I" + str(STAGE / "shader/glsl/__composed__"),
             "-I" + str(STAGE / "shader/glsl/pass"), *files, "-o", output],
            "link-" + directory.name)
        run(["spirv-val", "--target-env", "vulkan1.4", output], "validate-" + directory.name)
        link_reports.append({"name": directory.name, "stages": [path.suffix for path in files], "linked": True})
    cache = json.loads((STAGE / "shader/spv/shader-build-cache.json").read_text())
    validation_count = 0
    for record in cache["artifacts"].values():
        if "shaderName=terrain|" not in record["normalizedKey"] and "shaderName=pass/terrainFeedback|" not in record["normalizedKey"]:
            continue
        for role, output in record["outputs"].items():
            if not output["path"].endswith(".spv"):
                continue
            run(["spirv-val", "--target-env", "vulkan1.4", STAGE / "shader" / output["path"]],
                "spirv-" + str(validation_count))
            validation_count += 1
    run(["glslangValidator", "-V", "--target-env", "vulkan1.4",
         "-I" + str(STAGE / "shader/glsl"),
         ROOT / "tool/validation/terrain/rvt_geometry_coverage_bounded.comp",
         "-o", OUT / "bounded-coverage.spv"], "bounded-compile")
    run(["spirv-val", "--target-env", "vulkan1.4", OUT / "bounded-coverage.spv"], "bounded-validate")
    assert inventory(SOURCE / "shader") == baseline
    report = {"gpuStarted": False, "signedPackageChanged": False,
              "scope": "Private CPU shader test stage only; Pascal must export a fresh package",
              "stage": str(STAGE), "sourceDelta": str(target), "sourceSha256": digest(ROOT / target),
              "materialSchemaChanged": False, "tessTemplatesChanged": False,
              "additionalUnexportedTestSources": added_sources,
              "runtimeSpirvValidated": validation_count, "materialStageLinks": link_reports,
              "commands": commands, "baselineShaderInventory": baseline,
              "preparedShaderInventory": inventory(STAGE / "shader")}
    (OUT / "prepared.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if "Inventory" not in key and key != "commands"}, indent=2))


if __name__ == "__main__":
    main()
