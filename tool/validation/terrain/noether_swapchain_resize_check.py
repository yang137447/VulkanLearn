import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_black(path):
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise RuntimeError("Expected native BMP screenshot")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    if bits != 24 or width <= 0 or height == 0:
        raise RuntimeError("Unexpected native screenshot layout")
    stride = (width * 3 + 3) & ~3
    pixels = b"".join(data[offset + row * stride:offset + row * stride + width * 3]
                      for row in range(abs(height)))
    if len(pixels) != width * abs(height) * 3:
        raise RuntimeError("Truncated native screenshot")
    nonblack = sum(pixels[index:index + 3] != b"\0\0\0"
                   for index in range(0, len(pixels), 3))
    return {"path": str(path), "sha256": sha(path),
            "width": width, "height": abs(height), "nonblackPixels": nonblack}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true",
                        help="Start Vulkan only in an assigned GPU window")
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = root / "build-vt-release/source"
    work = Path(tempfile.gettempdir()) / "vulkanlearn-swapchain-owner-f1/harness"
    work.mkdir(parents=True, exist_ok=True)
    evidence = root / "artifacts/terrain-rvt-production/noether-swapchain-owner-f1"
    evidence.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("g++")
    if not compiler:
        raise RuntimeError("MinGW g++ is required")
    sources = [
        "source/vulkanManager.cpp",
        "source/render/rhi/vulkan/presentTarget.cpp",
        "source/render/rhi/vulkan/rhiDeviceVulkan.cpp",
        "source/render/backend/rendererBackendVulkan.cpp",
        "source/render/backend/rendererBackendVulkanPipelines.cpp",
        "source/render/frame/terrainCompositedLoading.cpp",
        "tool/validation/terrain/noether_swapchain_resize_check.cpp",
    ]
    headers = [
        "source/VulkanManager.h",
        "source/render/rhi/vulkan/presentTarget.h",
        "source/render/rhi/vulkan/rhiDeviceVulkan.h",
        "source/render/backend/rendererBackendVulkan.h",
        "source/render/frame/terrainCompositedLoading.h",
        "source/render/frame/terrainCompositedFrameLifetime.h",
        "source/terrain/terrainRvtBudget.h",
    ]
    inventory = {name: sha(root / name) for name in sources + headers}
    archive = build / "libvl_runtime.a"
    archive_hash = sha(archive)
    support = work / "support.a"
    shutil.copy2(archive, support)
    if sha(archive) != archive_hash or sha(support) != archive_hash:
        raise RuntimeError("Shared runtime archive changed during read-only snapshot")
    includes = work / "includes.rsp"
    shutil.copy2(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp", includes)
    previous_path = evidence / "build.json"
    previous = json.loads(previous_path.read_text("utf-8")) if previous_path.exists() else {}
    previous_inventory = previous.get("sourcesAndHeaders", {})
    flags = ["-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17",
             "-ffunction-sections", "-fdata-sections", "-msse2", "-mfpmath=sse",
             "@" + str(includes)]

    def compile_source(name):
        target = work / (Path(name).stem + ".obj")
        reusable = (target.exists()
                    and all(previous_inventory.get(header) == inventory[header]
                            for header in headers + [name])
                    and previous.get("objectSha256", {}).get(target.name) == sha(target)
                    and previous.get("includeResponseSha256") == sha(includes))
        if not reusable:
            command = [compiler, *flags, "-c", str(root / name), "-o", str(target)]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True)
            (work / (target.stem + ".log")).write_text(result.stdout + result.stderr, "utf-8")
            if result.returncode:
                raise RuntimeError("Compile failed: " + name + "\n" + result.stderr)
        print("CPU object ready: " + name, flush=True)
        return target

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        objects = list(pool.map(compile_source, sources))
    libraries = (build / "CMakeFiles/main_game.dir/linkLibs.rsp").read_text("utf-8")
    tokens = re.findall(r'"[^"]+"|\S+', libraries)
    resolved = []
    for token in tokens:
        value = token.strip('"')
        if value.startswith("-"):
            resolved.append(value)
        else:
            path = support if value == "libvl_runtime.a" else Path(value)
            if not path.is_absolute():
                path = (build / path).resolve()
            resolved.append('"' + path.as_posix() + '"')
    library_response = work / "libraries.rsp"
    library_response.write_text(" ".join(resolved), "utf-8")
    exe = work / "swapchain_resize_check.exe"
    command = [compiler, *map(str, objects), "-Wl,--gc-sections",
               "-Wl,-Map=" + str(work / "link.map"), "-o", str(exe),
               "@" + str(library_response)]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    (work / "link.log").write_text(result.stdout + result.stderr, "utf-8")
    result.check_returncode()
    if any(sha(root / name) != digest for name, digest in inventory.items()):
        raise RuntimeError("A source/header changed during the CPU build")
    manifest = {"scope": "Native resize owner harness; no Quality or P3 acceptance",
                "gpuExecuted": False, "sourcesAndHeaders": inventory,
                "compiler": compiler, "compileFlags": flags,
                "includeResponseSha256": sha(includes),
                "supportArchive": str(support), "supportArchiveSha256": sha(support),
                "objects": [str(path) for path in objects],
                "objectSha256": {path.name: sha(path) for path in objects},
                "executable": str(exe), "executableSha256": sha(exe),
                "testStubsLinked": False}
    previous_path.write_text(json.dumps(manifest, indent=2), "utf-8")
    print("CPU native link PASS: " + str(exe), flush=True)
    if not options.run:
        print("GPU not started; request an assigned window before --run.", flush=True)
        return
    output = (options.output or evidence / ("run-" + str(time.time_ns()))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ)
    environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
    with (output / "stdout.log").open("wb") as stdout, (
            output / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen([str(exe), str(output)], cwd=root, env=environment,
                                   stdout=stdout, stderr=stderr,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        launch = {"pid": process.pid, "argv": [str(exe), str(output)],
                  "startUnixSeconds": time.time(), "timeoutSeconds": 45,
                  "executableSha256": sha(exe), "gpuExecuted": True,
                  "PATH_prepend": str(root / "build-vt-release/bin")}
        (output / "launch.json").write_text(json.dumps(launch, indent=2), "utf-8")
        timed_out = False
        try:
            code = process.wait(timeout=45)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            code = process.wait(timeout=10)
    logs = (output / "stdout.log").read_text("utf-8", errors="replace") + (
        output / "stderr.log").read_text("utf-8", errors="replace")
    failures = [line for line in logs.splitlines()
                if re.search(r"VUID-|Validation Error|Validation Error:", line)]
    screenshots = [check_black(path) for path in sorted(output.glob("black-*.bmp"))]
    passed = (code == 0 and not timed_out and not failures and len(screenshots) == 4
              and all(image["nonblackPixels"] == 0 for image in screenshots))
    execution = {"pid": process.pid, "exitCode": code, "timedOut": timed_out,
                 "gpuProcessEnded": True, "validationErrors": failures,
                 "screenshots": screenshots, "moduleCheckPassed": passed,
                 "scope": "Swapchain resize/cache/fence/black-clear module only"}
    (output / "execution.json").write_text(json.dumps(execution, indent=2), "utf-8")
    print(json.dumps(execution, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
