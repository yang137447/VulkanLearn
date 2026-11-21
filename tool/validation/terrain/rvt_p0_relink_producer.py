"""Build the approved R32 format correction against frozen V1 producer objects."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    project = Path(__file__).resolve().parents[3]
    build = project / "build-vt-release/source"
    evidence = project / "artifacts/terrain-rvt-production/p0"
    frozen = evidence / "baseline-source/source"
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    original_header = frozen / "resource/image/hostImageVulkanPlan.h"
    override = output / "include/resource/image/hostImageVulkanPlan.h"
    override.parent.mkdir(parents=True)
    before = original_header.read_text(encoding="utf-8")
    anchor = "        case HostImage::PixelFormat::RGBA32_FLOAT:"
    replacement = ("        case HostImage::PixelFormat::R32_FLOAT:\n"
                   "            return vk::Format::eR32Sfloat;\n" + anchor)
    if before.count(anchor) != 1:
        raise ValueError("Frozen format mapping does not match the approved patch")
    after = before.replace(anchor, replacement).replace('"hostImage.h"',
        '"' + (frozen / "resource/image/hostImage.h").as_posix() + '"')
    override.write_text(after, encoding="utf-8")
    (output / "mapping.patch").write_text(
        "--- frozen/source/resource/image/hostImageVulkanPlan.h\n"
        "+++ isolated/source/resource/image/hostImageVulkanPlan.h\n@@\n"
        "+        case HostImage::PixelFormat::R32_FLOAT:\n"
        "+            return vk::Format::eR32Sfloat;\n"
        "         case HostImage::PixelFormat::RGBA32_FLOAT:\n", encoding="utf-8")
    archive = output / "libvl_runtime.a"
    shutil.copy2(evidence / "raw-cpu-link/libvl_runtime.a", archive)
    compiler, archiver = shutil.which("g++"), shutil.which("ar")
    includes = (evidence / "raw-cpu-link/includes.rsp").read_text()
    includes = '-I"' + (output / "include").as_posix() + '" ' + includes
    (output / "includes.rsp").write_text(includes)
    libraries = (build / "CMakeFiles/main_editor.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + archive.as_posix() + '"', 1)
    (output / "libraries.rsp").write_text(libraries)
    original_source = frozen / "resource/device/deviceTextureFactory.cpp"
    source = output / "deviceTextureFactory.cpp"
    source_text = original_source.read_text(encoding="utf-8")
    source_text = source_text.replace('"deviceTextureFactory.h"',
        '"' + (frozen / "resource/device/deviceTextureFactory.h").as_posix() + '"')
    source_text = source_text.replace('"../../commonFunction.h"',
        '"' + (frozen / "commonFunction.h").as_posix() + '"')
    source_text = source_text.replace('"../image/hostImageVulkanPlan.h"',
        '"' + override.as_posix() + '"')
    source.write_text(source_text, encoding="utf-8")
    commands = [
        [compiler, "-DIMATH_DLL", "-DNDEBUG", "-DOPENEXR_DLL",
         "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "@" + str(output / "includes.rsp"),
         "-O3", "-std=gnu++17", "-msse2", "-mfpmath=sse", "-c", str(source),
         "-o", str(output / "deviceTextureFactory.cpp.obj")],
        [archiver, "r", str(archive), str(output / "deviceTextureFactory.cpp.obj")],
        [compiler, "-O3", "-DNDEBUG", "-Wl,--whole-archive",
         "CMakeFiles/main_editor.dir/objects.a", "-Wl,--no-whole-archive",
         "-o", str(output / "main_editor.exe"), "@" + str(output / "libraries.rsp")],
    ]
    manifest = {"scope": "Approved isolated R32_FLOAT to eR32Sfloat mapping only; unchanged source precision and upload",
                "commands": commands, "cwd": str(build),
                "archiveBeforeSha256": digest(archive), "sourceSha256": digest(source),
                "originalSourceSha256": digest(original_source),
                "originalHeaderSha256": digest(original_header), "patchedHeaderSha256": digest(override)}
    for command in commands:
        subprocess.run(command, cwd=build, check=True)
    manifest["exeSha256"] = digest(output / "main_editor.exe")
    (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
